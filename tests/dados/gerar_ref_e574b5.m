% Referências MATLAB para os testes da E5.7.4b.5 (executa lawnumbers.m e malha_por_mascara.m
% originais, somente leitura). Saída: ref_e574b5.txt nesta pasta.
function gerar_ref_e574b5(refDir, outDir)
addpath(refDir);
out = fopen(fullfile(outDir, 'ref_e574b5.txt'), 'w');
tmpFid = fopen(fullfile(outDir, 'lixo_apdl.txt'), 'w');

% ---- constantes (DEM_HIAGO_PRINCIPAL.m l. 123-131, valores padrão)
LCO = 1e-3; PSN = 0.36; YNG = 3.5e9; GRF = 2400; RFC = 10; CORRCAF = 1;
CAF = 0.1385 * CORRCAF;
POA = 1-PSN^2;
delta = 9*PSN/(4-8*PSN);
alfa = (9+8*delta)/(18+24*delta);
LCR = 2*CAF*POA/(alfa*RFC^2);
EPR = RFC*sqrt(GRF/(YNG*POA));
ENR = YNG * LCO^2 * alfa;
ENRD = YNG * (LCO*sqrt(3)/2)^2 * alfa;
EDG = 2*delta*(4/3)*(ENRD /sqrt(3));
fprintf(out, 'CONST %.17g %.17g %.17g %.17g %.17g\n', LCR, EPR, ENR, EDG, LCO*1e-6);

% ---- jj = round(k,-1)+10
fprintf(out, 'JJ');
for k = 0:200
    fprintf(out, ' %d', round(k,-1)+10);
end
fprintf(out, '\n');

% ---- classes de Weibull (GF_field.m l. 38-67, copiadas literalmente)
for caso = 1:3
    switch caso
        case 1, Cv = 0.8; inter = 20;
        case 2, Cv = 0.3; inter = 10;
        case 3, Cv = 0.5; inter = 2;
    end
    x = zeros(1,inter); xm = zeros(1,inter);
    GM=35.;
    err=1;
    DG=0.01;
    while abs(err)~=0
        if err>0
            GM=GM-DG;
        end
        if err<0
            GM=GM+DG;
            DG=DG/2;
        end
        DE=Cv^2+1-(gamma(1+2/GM))/(gamma(1+1/GM))^2;
        err=fix(10000*DE);
    end
    BT=gamma(1+1/GM);
    i=1;
    while i<inter
        x(i)=((log(1/((inter-i)/inter)))^(1/GM)/BT)^0.5;
        if(i==1)
            xm(i)=(x(i)+((log(1/(0.999)))^(1/GM)/BT)^0.5)/2;
        else
            xm(i)=(x(i)+x(i-1))/2;
        end
        i=i+1;
    end
    xm(inter)=(x(inter-1)+(log(1/(0.001))^(1/GM)/BT)^0.5)/2;
    fprintf(out, 'WEIBULL %g %d %.17g %.17g', Cv, inter, GM, BT);
    fprintf(out, ' X'); fprintf(out, ' %.17g', x(1:inter-1));
    fprintf(out, ' XM'); fprintf(out, ' %.17g', xm);
    fprintf(out, '\n');
    if caso == 1
        xm20 = xm;
    end
end

% ---- casos sintéticos diretos em lawnumbers.m
tol = LCO*1e-6;
d = LCO*sqrt(3)/2;
% S1: tolerância não transitiva, fronteira exata, tp obrigatório, tp = 3, comprimentos diferentes
gf = [1, 1+1.5e-9, 1+0.8e-9, 0, tol, 1, 1, 1, 1, 0.7, 0.7, 1, 1];
tp = [4, 4,        4,        4, 4,   3, 3, 0, 1, 2,   2,   4, 4];
L  = [LCO,LCO,     LCO,      LCO,LCO,LCO,LCO,d,LCO,LCO,1.2*LCO,1.2*LCO,LCO];
escreveCaso(out, 'S1', tp, L, gf, EPR, LCO, LCR, ENR, EDG, tmpFid, true);

% ---- casos sobre a rede (malha_por_mascara.m)
ROH = 1240;
caixa = true(4,4,2);
ele = true(4,4,2); ele(3:4,3:4,:) = false;        % seção em L (canto reentrante: tp = 3)
cubos = false(3,3,3); cubos(1,1,1) = true; cubos(2,2,1) = true; cubos(2,2,2) = true; cubos(3,1,2) = true;
mascaras = {caixa, ele, cubos};
nomes = {'caixa', 'ele', 'cubos'};
for m = 1:numel(mascaras)
    [~, BONDLIST, tpm, ~, nBonds, ~, LEN] = malha_por_mascara(mascaras{m}, LCO, ROH, tmpFid); %#ok<ASGLU>
    % Gf = 1 (CVgf = 0, GF_field.m l. 278)
    escreveCaso(out, [nomes{m} '_g1'], tpm, LEN, ones(1,nBonds), EPR, LCO, LCR, ENR, EDG, tmpFid, false);
    % Gf com padrão determinístico sobre as classes xm (CVgf = 0,8, inter = 20)
    gfp = xm20(mod(7*(1:nBonds), 20) + 1);
    escreveCaso(out, [nomes{m} '_xm'], tpm, LEN, gfp, EPR, LCO, LCR, ENR, EDG, tmpFid, false);
end

fclose(tmpFid);
fclose(out);
end

function escreveCaso(out, nome, tp, L, gf, EPR, LCO, LCR, ENR, EDG, fid, porBarra)
tic
nBonds = numel(tp);
[EEP,FP,EEU,kr,mat_bonds] = lawnumbers(tp(:),EPR,nBonds,L(:),LCO,LCR,gf,ENR,EDG,fid);
k = max(mat_bonds);
fprintf(out, 'CASE %s %d %d %d\n', nome, nBonds, k, round(k,-1)+10);
fprintf(out, 'TP'); fprintf(out, ' %d', tp); fprintf(out, '\n');
fprintf(out, 'MAT'); fprintf(out, ' %d', mat_bonds); fprintf(out, '\n');
for j = 1:k
    b = find(mat_bonds == j, 1);
    fprintf(out, 'LAW %d %d %d %.17g %.17g %.17g %.17g %.17g %.17g\n', j, tp(b), b, gf(b), L(b), kr(b), EEP(b), FP(b), EEU(b));
end
if porBarra
    for b = 1:nBonds
        fprintf(out, 'BOND %d %.17g %.17g %.17g %.17g\n', b, kr(b), EEP(b), FP(b), EEU(b));
    end
end
end
