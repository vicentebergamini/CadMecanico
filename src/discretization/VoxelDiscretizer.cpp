#include "discretization/VoxelDiscretizer.h"

#include "discretization/CubeClassifier.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace cad::discretization {

namespace {

using Clock = std::chrono::steady_clock;

double seconds(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}

// Blocos com até esta quantidade de células recebem o teste preciso de atravessamento; blocos
// maiores perto do contorno são subdivididos sem teste (o teste quase sempre seria positivo).
constexpr std::int64_t kPreciseRegionCells = 512;
// Fração do progresso atribuída à classificação (o restante é a numeração).
constexpr double kClassificationShare = 0.9;

struct Region
{
    int i0, i1, j0, j1, k0, k1; // intervalos semiabertos de índices de células

    std::int64_t cells() const { return std::int64_t(i1 - i0) * (j1 - j0) * (k1 - k0); }
};

class Worker
{
public:
    Worker(const VoxelGrid& grid, const CubeClassifier& classifier, InclusionCriterion criterion,
           std::vector<std::uint8_t>& accepted, DiscretizationStats& stats, const ProgressCallback& progress)
        : m_grid(grid), m_cls(classifier), m_criterion(criterion), m_accepted(accepted), m_stats(stats),
          m_progress(progress), m_total(grid.cellCount())
    {
    }

    bool cancelled() const { return m_cancelled; }

    void classify(const Region& r, const std::vector<int>& candidates)
    {
        if (m_cancelled)
            return;
        const double d = m_cls.shrink();
        const gp_XYZ pmin(m_grid.coordinate(0, 2 * std::int64_t(r.i0)) + d, m_grid.coordinate(1, 2 * std::int64_t(r.j0)) + d,
                          m_grid.coordinate(2, 2 * std::int64_t(r.k0)) + d);
        const gp_XYZ pmax(m_grid.coordinate(0, 2 * std::int64_t(r.i1)) - d, m_grid.coordinate(1, 2 * std::int64_t(r.j1)) - d,
                          m_grid.coordinate(2, 2 * std::int64_t(r.k1)) - d);
        std::vector<int> near;
        m_cls.facesNear(pmin, pmax, candidates, near);
        const std::int64_t cells = r.cells();
        if (near.empty())
        {
            uniform(r);
            return;
        }
        if (cells <= kPreciseRegionCells)
        {
            // Prova barata e exata de atravessamento: pontos do bloco com estados diferentes (ou
            // um ponto sobre a superfície). Se todos concordam, o teste de distância decide (ele
            // detecta furos e cavidades que passam entre os pontos amostrados).
            PointState agreed = PointState::On;
            bool crosses = mixedSamples(pmin, pmax, agreed);
            if (!crosses)
            {
                long long tests = 0, failures = 0;
                const auto t0 = Clock::now();
                crosses = m_cls.boundaryCrosses(pmin, pmax, near, &tests, &failures);
                m_stats.boundarySeconds += seconds(t0, Clock::now());
                m_stats.boundaryTests += tests;
                m_stats.extremaFailures += failures;
            }
            if (!crosses)
            {
                uniform(r, agreed);
                return;
            }
            if (cells == 1)
            {
                boundaryCube(r.i0, r.j0, r.k0);
                return;
            }
        }
        // Subdivide o maior lado (em número de células); a ordem das metades é fixa.
        const int di = r.i1 - r.i0, dj = r.j1 - r.j0, dk = r.k1 - r.k0;
        Region a = r, b = r;
        if (di >= dj && di >= dk)
            a.i1 = b.i0 = r.i0 + di / 2;
        else if (dj >= dk)
            a.j1 = b.j0 = r.j0 + dj / 2;
        else
            a.k1 = b.k0 = r.k0 + dk / 2;
        classify(a, near);
        classify(b, near);
    }

private:
    // Centro e cantos do bloco encolhido: true se os estados diferem ou algum ponto está sobre a
    // superfície (o contorno passa pelo bloco). Caso contrário 'agreed' recebe o estado comum.
    bool mixedSamples(const gp_XYZ& pmin, const gp_XYZ& pmax, PointState& agreed)
    {
        agreed = state(0.5 * (pmin + pmax));
        if (agreed == PointState::On)
            return true;
        for (int c = 0; c < 8; ++c)
        {
            const gp_XYZ p((c & 1) ? pmax.X() : pmin.X(), (c & 2) ? pmax.Y() : pmin.Y(), (c & 4) ? pmax.Z() : pmin.Z());
            if (state(p) != agreed)
                return true;
        }
        return false;
    }

    // Bloco sem contorno no interior: todas as células têm o estado de qualquer ponto interior
    // ('known' = estado já classificado de um ponto do bloco, On = desconhecido).
    void uniform(const Region& r, PointState known = PointState::On)
    {
        ++m_stats.uniformRegions;
        PointState s = known != PointState::On ? known : state(m_grid.center(r.i0, r.j0, r.k0));
        if (s == PointState::On && r.cells() > 1)
            s = state(m_grid.center(r.i1 - 1, r.j1 - 1, r.k1 - 1));
        if (s == PointState::In)
            for (int k = r.k0; k < r.k1; ++k)
                for (int j = r.j0; j < r.j1; ++j)
                {
                    const std::int64_t base = m_grid.cellIndex(r.i0, j, k);
                    std::fill(m_accepted.begin() + base, m_accepted.begin() + base + (r.i1 - r.i0), std::uint8_t(1));
                }
        advance(r.cells());
    }

    // Cubo atravessado pelo contorno do sólido (contém material e vazio).
    void boundaryCube(int i, int j, int k)
    {
        ++m_stats.boundaryCubes;
        bool accept = false;
        switch (m_criterion)
        {
        case InclusionCriterion::FullyInside: accept = false; break;
        case InclusionCriterion::PositiveIntersection: accept = true; break;
        case InclusionCriterion::CenterInside: accept = state(m_grid.center(i, j, k)) == PointState::In; break;
        }
        if (accept)
            m_accepted[std::size_t(m_grid.cellIndex(i, j, k))] = 1;
        advance(1);
    }

    PointState state(const gp_XYZ& p)
    {
        ++m_stats.pointClassifications;
        const auto t0 = Clock::now();
        const PointState s = m_cls.classify(p);
        m_stats.pointSeconds += seconds(t0, Clock::now());
        return s;
    }

    void advance(std::int64_t cells)
    {
        m_done += cells;
        if (!m_progress)
            return;
        const auto now = Clock::now();
        if (m_done < m_total && now - m_lastReport < std::chrono::milliseconds(50))
            return;
        m_lastReport = now;
        if (!m_progress(kClassificationShare * double(m_done) / double(std::max<std::int64_t>(1, m_total))))
            m_cancelled = true;
    }

    const VoxelGrid& m_grid;
    const CubeClassifier& m_cls;
    InclusionCriterion m_criterion;
    std::vector<std::uint8_t>& m_accepted;
    DiscretizationStats& m_stats;
    const ProgressCallback& m_progress;
    std::int64_t m_total = 0;
    std::int64_t m_done = 0;
    Clock::time_point m_lastReport{};
    bool m_cancelled = false;
};

std::string formatBytes(std::size_t bytes)
{
    std::ostringstream s;
    s.precision(1);
    s << std::fixed << double(bytes) / (1024.0 * 1024.0) << " MiB";
    return s.str();
}

} // namespace

GridEstimate VoxelDiscretizer::estimate(const TopoDS_Shape& shape, double lco)
{
    GridEstimate e;
    if (!(lco > 0.0) || !std::isfinite(lco))
    {
        e.message = "O comprimento da aresta lco deve ser maior que zero";
        return e;
    }
    if (shape.IsNull())
    {
        e.message = "Nenhum sólido selecionado";
        return e;
    }
    for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next())
        ++e.solids;
    if (e.solids == 0)
    {
        e.message = "A forma selecionada não contém sólido";
        return e;
    }
    CubeClassifier probe(shape, lco);
    if (probe.box().IsVoid())
    {
        e.message = "Não foi possível determinar os limites geométricos do sólido";
        return e;
    }
    double x0, y0, z0, x1, y1, z1;
    probe.box().Get(x0, y0, z0, x1, y1, z1);
    e.boxMin = gp_XYZ(x0, y0, z0);
    e.boxMax = gp_XYZ(x1, y1, z1);
    e.grid = VoxelGrid::make(e.boxMin, lco);
    double counts[3];
    for (int a = 0; a < 3; ++a)
    {
        const double extent = e.boxMax.Coord(a + 1) - e.grid.origin.Coord(a + 1);
        // Tolerância relativa: um excesso de até 1e-6·lco (ruído da caixa) não cria uma camada nova.
        counts[a] = std::max(1.0, std::ceil(extent / lco - 1e-6));
    }
    const double cells = counts[0] * counts[1] * counts[2];
    if (counts[0] > 2.0e9 || counts[1] > 2.0e9 || counts[2] > 2.0e9 || cells > double(kMaxCells))
    {
        std::ostringstream s;
        s.precision(0);
        s << std::fixed << "Grade grande demais: " << counts[0] << " × " << counts[1] << " × " << counts[2] << " = "
          << cells << " cubos candidatos (limite " << kMaxCells << "). Aumente lco.";
        e.message = s.str();
        return e;
    }
    e.grid.nx = int(counts[0]);
    e.grid.ny = int(counts[1]);
    e.grid.nz = int(counts[2]);
    e.cells = e.grid.cellCount();
    e.latticePoints = e.grid.latticeCount();
    // Mapa de aceitação (1 B/célula) + bits + IDs da rede (4 B/ponto) + listas de cubos e vértices
    // (8 B cada, pior caso).
    e.estimatedBytes = std::size_t(e.cells) * (1 + 8) + std::size_t(e.cells) / 8
                       + std::size_t(e.latticePoints) * (4 + 8);
    e.ok = true;
    e.message = "Grade " + std::to_string(e.grid.nx) + " × " + std::to_string(e.grid.ny) + " × "
                + std::to_string(e.grid.nz) + " = " + std::to_string(e.cells) + " cubos candidatos (memória estimada "
                + formatBytes(e.estimatedBytes) + ")";
    return e;
}

DiscretizeResult VoxelDiscretizer::run(const TopoDS_Shape& shape, const Parameters& parameters,
                                       const ProgressCallback& progress)
{
    DiscretizeResult r;
    const auto t0 = Clock::now();
    const GridEstimate est = estimate(shape, parameters.lco);
    if (!est.ok)
    {
        r.message = est.message;
        return r;
    }
    auto data = std::make_shared<Discretization>();
    DiscretizationStats& stats = data->stats();
    try
    {
        GProp_GProps props;
        BRepGProp::VolumeProperties(shape, props);
        stats.solidVolume = std::fabs(props.Mass());

        CubeClassifier classifier(shape, parameters.lco);
        std::vector<std::uint8_t> accepted(std::size_t(est.cells), 0);
        Worker worker(est.grid, classifier, parameters.criterion, accepted, stats, progress);
        worker.classify(Region{0, est.grid.nx, 0, est.grid.ny, 0, est.grid.nz}, classifier.allFaces());
        const auto t1 = Clock::now();
        stats.classificationSeconds = seconds(t0, t1);
        if (worker.cancelled())
        {
            r.cancelled = true;
            r.message = "Discretização cancelada pelo usuário";
            return r;
        }
        auto keepGoing = [&progress] { return !progress || progress(kClassificationShare + 0.05); };
        if (!data->build(est.grid, parameters, accepted, keepGoing))
        {
            r.cancelled = true;
            r.message = "Discretização cancelada pelo usuário";
            return r;
        }
        const auto t2 = Clock::now();
        stats.numberingSeconds = seconds(t1, t2);
        stats.totalSeconds = seconds(t0, t2);
    }
    catch (const Standard_Failure& e)
    {
        r.message = std::string("Falha geométrica na discretização: ") + e.what();
        return r;
    }
    catch (const std::bad_alloc&)
    {
        r.message = "Memória insuficiente para a discretização; aumente lco";
        return r;
    }
    if (progress)
        progress(1.0);
    r.ok = true;
    r.data = std::move(data);
    if (r.data->empty())
        r.message = "Nenhum cubo satisfaz o critério de inclusão com este lco";
    return r;
}

} // namespace cad::discretization
