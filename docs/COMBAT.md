# Anexo 08 — Combate abstrato, perdas, moral e retirada

> **Status:** especificação técnica proposta (v0.1, 17/11/2026). Os princípios
> já decididos são obrigatórios; números e modelos indicados como *default*
> são hipóteses de implementação a validar em simulações. Este anexo **não**
> autoriza antecipar marcos. `DESIGN.md` e `TERRITORY.md` prevalecem nas
> responsabilidades já especificadas; conflitos devem ser registrados, não
> resolvidos silenciosamente.

**Regras transversais.** C++20 simples e convencional, núcleo headless, uma
fonte autoritativa de estado, relógio simulado, seed configurável, iteração
estável por ID, eventos com `(tick, phase, sequence, entity_id)`, IDs explícitos,
reprodutibilidade após save/load, nenhum vínculo com FPS. As interfaces abaixo
são contratos conceituais; assinaturas C++ definitivas devem seguir o código
existente, sem frameworks ou hierarquias antecipadas.

## 1. Âmbito

Resolver encontros **fora da bolha tática** de maneira barata e
reproduzível, sem física de projéteis. Um combate deve produzir danos,
munição consumida, desgaste, perdas, observações plausíveis e mudanças de
postura que permanecem no estado da campanha. Captura de setor é
responsabilidade de `TERRITORY.md`, nunca efeito colateral automático de dano.

## 2. Estado das unidades

`UnitCombatState`: unit_id, strength, equipment_condition, ammo_by_class,
fuel, morale, cohesion, fatigue, experience, suppression, posture,
retreat_order, location, status. Valores normalizados 0..100 para moral,
coesão, fadiga e supressão na v0; força/equipamento possuem medidas próprias.

**Separações indispensáveis:** moral = vontade de continuar; coesão = capacidade
de atuar coordenadamente; fadiga = desgaste; experiência = habilidade adquirida.
Distinguir `BROKEN` (desorganizada), `RETREATING` (retirada), `SURRENDERED`
(rendida) e `DESTROYED` (destruída). Uma unidade pode recuar com disciplina
mesmo tendo moral baixo.

## 3. Entrada de um encontro

`Engagement`: id, location, start_tick, participants, known_targets,
terrain_context, posture, duration_budget, resolved_until_tick.
Criação exige contato/condição operacional plausível. Observação de alvo é
separada da capacidade de acertá-lo; camadas de comando não conhecem por
magia todos os participantes reais. A entidade real resolve impactos; a IA
planeja somente com sua inteligência.

## 4. Algoritmo abstrato inicial

A cada intervalo de combate:

1. Ordenar participantes por ID; identificar pares com detecção e alcance
   abstrato válidos, terreno/postura e armamento utilizável.
2. Limitar ataques por munição, prontidão e taxa de emprego do armamento.
3. Calcular poder efetivo explícito a partir de equipamento, treinamento,
   terreno, condição, moral, coesão, fadiga e supressão; limitar resultados.
4. Sortear efeitos apenas com RNG controlado, com ordem de chamadas estável;
   separar streams por subsistema quando implementado.
5. Aplicar danos e consumo como transação única, registrar vítimas/perdas e
   eventos. Evitar que combate seja aplicado novamente no intervalo tático.
6. Atualizar moral, coesão, fadiga e supressão; avaliar retirada ou rendição.
7. Emitir resultados para TERRITORY, MAINTENANCE, INTELLIGENCE e OPERATIONS.

A v0 pode usar uma fórmula simples, por exemplo
`effectiveness = base_power * readiness_factor * cohesion_factor *
fatigue_factor * terrain_factor`, com fatores e intervalos parametrizados.
Não atribuir probabilidades históricas sem validação; fixar regra que seja
reprodutível, conservadora em recursos e explicável pelos logs. Um exemplo
**não é** calibração definitiva de letalidade.

## 5. Moral e retirada

Eventos negativos (perdas, cerco, escassez, ataque inesperado) reduzem moral;
descanso, reabastecimento, reforços e vitórias verificadas podem recuperá-la.
Coesão piora sob supressão/perdas e recupera em reorganização. Fadiga cresce
com operações e diminui em repouso. Modificadores devem ser combinados uma
vez por tick e limitados a 0..100, sem abruptas mudanças não justificadas.

Retirada usa avaliação de risco e possibilidade de rota; fugir não pode
teletransportar. `SURRENDERED` só ocorre com regra explícita de impossibilidade
ou decisão de rendição, não por moral abaixo de um limiar isolado. Uma unidade
sem rota pode resistir, render-se ou sofrer derrota conforme circunstâncias.
Comportamento deve ser estocástico **apenas** se seed determinística controlar
os sorteios; fornecer explicação no journal.

## 6. Defesa aérea e helicópteros

Armas por classe: canhão AA, MANPADS, SAM curto/médio/longo e armas aéreas.
Primeira versão usa cobertura, disponibilidade, munição, detecção e perfil de
alvo; não modelar guiagem de mísseis real. Radar e MANPADS não podem atacar
alvo que não detectaram/receberam por relatório válido. Em combate aproximado,
o TACTICAL assume a simulação detalhada segundo contrato de handoff. Sem
pontuação unilateral: defensores e atacantes gastam munição, sofrem perdas e
podem se retirar.

## 7. Fronteira entre sistemas

- TERRITORY aplica ocupação pós-combate, nunca por `enemy_strength == 0` só.
- ECONOMY contabiliza estoque externo; combate consome munição já entregue à
  unidade, sem debitar simultaneamente o depósito da base.
- MAINTENANCE recebe dano persistente de veículos individuais.
- INTELLIGENCE recebe somente observações com fonte autorizada.
- OPERATIONS recebe resultado, perda de meios e novas restrições.
- TACTICAL substitui resolução abstrata das entidades que possuem autoridade
  tática naquele intervalo de tempo.

Eventos: `ENGAGEMENT_STARTED`, `AMMO_EXPENDED`, `UNIT_DAMAGED`,
`UNIT_SUPPRESSED`, `UNIT_RETREATING`, `UNIT_SURRENDERED`,
`UNIT_DESTROYED`, `ENGAGEMENT_ENDED`.

## 8. Testes

- C01 encontro sem arma/munição compatível não produz dano gratuito.
- C02 ataque debita munição da unidade uma única vez.
- C03 perdas reduzem força/inventário e impactam moral/cohesion.
- C04 unidade desmoralizada pode recuar, sem desaparecimento instantâneo.
- C05 retirada segue rota válida, preservando identidade da unidade.
- C06 aniquilação de defensor não transfere setor automaticamente.
- C07 defesa antiaérea sem detecção não ataca alvo oculto.
- C08 mesma seed e entradas geram resultado idêntico e journal idêntico.
- C09 save/load no meio do combate não aplica danos duas vezes.
- C10 entidades controladas pelo TACTICAL não recebem dano abstrato simultâneo.

## 9. Etapas e aberturas

C0 estado e evento de dano; C1 resolução abstrata terrestre; C2 moral,
retirada e rendição; C3 ameaça antiaérea e integração tática. Em aberto:
calibração dos armamentos, terreno e coberturas, escala de formação, moral por
comandante, prisioneiros e resultados de combate contra instalações.
