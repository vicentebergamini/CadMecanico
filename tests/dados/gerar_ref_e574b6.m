% Referências textuais MATLAB para os testes da E5.7.4b.6: executa lawnumbers.m original (somente
% leitura) em casos sintéticos e grava, para cada caso, as entradas (%.17g) e o texto APDL exato que
% lawnumbers.m escreve (blocos de leis e EMODIF). Saída: matlab_e574b6_ref.txt nesta pasta.
function gerar_ref_e574b6(refDir, outDir)
addpath(refDir);
out = fopen(fullfile(outDir, 'matlab_e574b6_ref.txt'), 'w');

% Constantes (DEM_HIAGO_PRINCIPAL.m l. 123-131, valores padrão) para cada LCO.
caso(out, 'uma_lei',      1e-3, 4,            1e-3*[1],                1);
caso(out, 'multiplas',    1e-3, [1 2 4 0],    1e-3*[1 1 1 sqrt(3)/2],  [1 1 1 1]);
caso(out, 'compartilhada',1e-3, [4 4 4 4 4 4], 1e-3*ones(1,6),         ones(1,6));
caso(out, 'tp3_nula',     1e-3, [3 4 3 1],    1e-3*[1 1 1 1],          [1 1 1 1]);
caso(out, 'gf_zero',      1e-3, [4 4 0],      1e-3*[1 1 sqrt(3)/2],    [0 1 0]);
caso(out, 'comprimentos', 1e-3, [4 4 2 2],    1e-3*[1 1.2 1 0.8],      [1 1 0.7 0.7]);
% Caso S1 da E5.7.4b.5 (tolerância não transitiva, fronteira, tp = 3, Gf = 0, comprimentos).
LCO = 1e-3; tol = LCO*1e-6; d = LCO*sqrt(3)/2;
caso(out, 'S1', LCO, [4 4 4 4 4 3 3 0 1 2 2 4 4], ...
     [LCO LCO LCO LCO LCO LCO LCO d LCO LCO 1.2*LCO 1.2*LCO LCO], ...
     [1 1+1.5e-9 1+0.8e-9 0 tol 1 1 1 1 0.7 0.7 1 1]);
% Limites de jj: k = 4, 5, 14, 15, 45, 95 leis (Gf distintos, tp = 4), com repetições intercaladas.
for k = [4 5 14 15 45 95]
    gf = [1 + 0.01*(1:k), 1 + 0.01*(k:-1:1)];
    caso(out, sprintf('jj_k%d', k), 1e-3, 4*ones(1,2*k), 1e-3*ones(1,2*k), gf);
end
% Valores pequenos e grandes em notação científica (LCO = 1 µm e LCO = 1 m; Gf pequeno).
caso(out, 'pequenos', 1e-6, [1 2 4 0], 1e-6*[1 1 1 sqrt(3)/2], [1e-3 1 1 0.5]);
caso(out, 'grandes',  1,    [1 4 0],   [1 1 sqrt(3)/2],         [1 1 1]);
fclose(out);
end

function caso(out, nome, LCO, tp, L, gf)
PSN = 0.36; YNG = 3.5e9; GRF = 2400; RFC = 10; CORRCAF = 1;
CAF = 0.1385 * CORRCAF;
POA = 1-PSN^2;
delta = 9*PSN/(4-8*PSN);
alfa = (9+8*delta)/(18+24*delta);
LCR = 2*CAF*POA/(alfa*RFC^2);
EPR = RFC*sqrt(GRF/(YNG*POA));
ENR = YNG * LCO^2 * alfa;
ENRD = YNG * (LCO*sqrt(3)/2)^2 * alfa;
EDG = 2*delta*(4/3)*(ENRD /sqrt(3));

nBonds = numel(tp);
tmp = [tempname '.txt'];
fid = fopen(tmp, 'w');
tic
lawnumbers(tp(:), EPR, nBonds, L(:), LCO, LCR, gf, ENR, EDG, fid);
fclose(fid);
fid = fopen(tmp, 'r');
texto = fread(fid, inf, '*char')';
fclose(fid);
delete(tmp);

fprintf(out, '#CASE %s %d\n', nome, nBonds);
fprintf(out, '#CONST %.17g %.17g %.17g %.17g %.17g\n', LCO, LCR, EPR, ENR, EDG);
fprintf(out, '#TP'); fprintf(out, ' %d', tp); fprintf(out, '\n');
fprintf(out, '#L');  fprintf(out, ' %.17g', L); fprintf(out, '\n');
fprintf(out, '#GF'); fprintf(out, ' %.17g', gf); fprintf(out, '\n');
fprintf(out, '#BEGIN %d\n', numel(texto));
fwrite(out, texto, 'char');
fprintf(out, '#END\n');
end
