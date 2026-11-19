# Anexo 06 — Sensores, contatos e inteligência de facção

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

## 1. Autoridade e limites

Este anexo **estende**, sem substituir, as regras de observação, obsolescência
e `FactionView` de `TERRITORY.md`. O estado real pertence ao `WorldState`;
cada facção mantém `IntelStore` próprio. UI e IA de facção consultam somente
`FactionView`; um modo debug autorizado pode consultar a verdade. Em momento
algum a mudança do controlador real atualiza automaticamente o mapa inimigo.

## 2. Tipos de informação

- `SectorReport`: sector_id, estimated_controller, observed_at, confidence,
  source_id, flags (contested, damaged, suspected).
- `ContactReport`: contact_id, estimated_position, uncertainty_radius,
  classification (unknown, aircraft, ground, SAM, etc.), identification
  (optional), confidence, observed_at, source_id, estimated_strength.
- `InstallationReport`: id_or_estimated_location, type_guess, integrity_guess,
  operational_guess, confidence, observed_at.
- `Observation`: observer_faction, sensor_id, tick, target_ref_if_known,
  reported_data, quality, visibility_context.

Identidades reais de unidades inimigas **não** devem ser expostas por `ContactId`.
A associação contato-entidade é interna ao subsistema de observação. Contatos
podem duplicar ou ser confundidos até ocorrer fusão verificável.

## 3. Sensores (modelo abstrato inicial)

`SensorProfile`: type (visual, radar, radio_report, reconnaissance), range,
field_of_view_optional, revisit_interval, classification_quality,
detection_chance, active/passive. Na v0, localização e distância entre setores
são suficientes; relevo, LOS, interferência e tempo serão introduzidos
posteriormente, sempre por regra determinística. Radar não equivale a saber
posição/tipo de todos os alvos; sua cobertura deve ser explicitamente limitada.

Ciclo: construir candidatos reais dentro de alcance interno; avaliar sensor,
condição e RNG por ordem estável; gerar `Observation` para a facção observadora;
fundir no `IntelStore`; degradar contatos sem novas observações; publicar apenas
`FactionView`. A camada interna pode conhecer a verdade para calcular detecção,
mas o planejador jamais consulta essa camada.

## 4. Atualização e decaimento

Priorizar os defaults já estabelecidos em `TERRITORY.md`: half-life territorial
360 min, contatos móveis 30 min, instalações 1440 min; limiar de incerto 0,75
e desconhecido 0,35, configuráveis. Exemplificação:
`confidence(t) = initial_confidence * 2^(-elapsed_minutes / half_life)`.
Para determinismo multiplataforma, definir política de arredondamento e formato
numérico; alternativa v0: lookup/tabela discretizada por minuto. Confiança
não pode ser recalculada sobre valor já decaído se elapsed se refere sempre à
observação original (evitar duplo decaimento).

Sem observação nova, posição estimada permanece última localização conhecida,
com raio de incerteza crescente; nunca seguir silenciosamente a unidade real.
Conhecimento de território amigo próprio pode resultar de relatórios internos,
mas deve existir como fonte explícita de informação.

## 5. Fusão e contradições

### Geographic road reports

Ground formations and carriers report the condition they observe when trying
to use a road or bridge. Each report belongs to that faction and carries the
segment ID, the last observed condition and observation tick. A newly
discovered closure or a changed condition generates a faction operations event.
Road confidence uses the existing installation half-life and decay. Below
750/1000, planning treats the condition as unknown while the view retains
the dated report. A missing report means the faction has never observed that
segment. Changing actual availability updates campaign state, and later
passage updates the observing faction's report.

Observações com alvo comprovadamente idêntico atualizam mesmo contato;
observações com identidade incerta podem permanecer separadas. Informações
contraditórias mantêm timestamp, qualidade e trilha de fontes; não sobrescrever
relatório recente confiável com relatório antigo de baixa qualidade. A v0 pode
adotar regra determinística simples: maior `observed_at`, depois confiança,
depois `source_id`, e adicionar trilha de auditoria. Evitar qualquer verdade
oculta para desempatar o que a facção acredita.

### Ground reconnaissance reports

Ground Recon uses the existing faction intelligence store and its confidence
and aging rules. Geography visible in planning remains the static map and
the faction's reported road and sector conditions. A hidden contact can
interrupt a mission in execution. The draft and preview use reported data.

Arrival refreshes the objective's territorial report. After one observation
tick, an open objective can produce current contact and installation
reports using the existing sector-level detection filters. Forest at the
sector or objective tile limits the mission to a territorial report.
Contact-free observation leaves the objective without a new complete survey.
Earlier complete survey timestamps still govern when older contacts were
superseded. Incidental observation from other movement crossings and
engagements follows its existing rules. Reports remain with the observing
faction, including when a later interruption ends the mission.

Ground Patrol reports territorial conditions on sector crossings and
observes at each waypoint it reaches. Each visit uses the same open or
forested coverage as Ground Recon without a dedicated observation tick.
Combat can interrupt observation. Reports belong to the patrolling
faction. An observation with no detections does not establish sector clearance.

## 6. Relatórios e comunicações

Uma observação pode ser publicada imediatamente na v0 (default configurável),
mas modelo precisa suportar `available_at_tick` para atraso de comunicação.
Unidade isolada pode manter informação local até transmissão; se destruída,
relatório ainda não enviado pode perder-se. Comunicação interrompida afeta
conhecimento compartilhado, não o controlador verdadeiro do setor.

## 7. Integrações

- TERRITORY fornece controladores verdadeiros apenas ao sensor interno;
  `FactionView` estima controle por relatórios.
- OPERATIONS recebe observações e classificação de ameaças, sem posições
  reais secretas; alvos desconhecidos podem gerar reconhecimento.
- COMBAT fornece evento observado/relatório de batalha somente aos atores
  autorizados; um combate não informa automaticamente o inimigo remoto.
- TACTICAL produz observações de sensores próximos sem duplicar contato.
- O mapa 2D exibe último contato e grau de incerteza, não entidade real.

Eventos: `SENSOR_DETECTED`, `OBSERVATION_REPORTED`, `INTEL_UPDATED`,
`CONTACT_STALE`, `CONTACT_LOST`, `REPORT_DELAYED`.

## 8. Testes

- I01 mudança de setor sem observador não altera IntelStore adversário.
- I02 último contato móvel não acompanha movimento real sem atualização.
- I03 confiança segue meia-vida configurada, sem degradação duplicada.
- I04 contato desconhecido não revela ID/nome/tipo exato do alvo.
- I05 operador de WEST não acessa `IntelStore` de EAST via API normal.
- I06 observação repetida atualiza contato de forma determinística.
- I07 relatório atrasado só fica visível no tick de chegada.
- I08 radar fora de alcance não detecta automaticamente o alvo.
- I09 alvo atacado em localização antiga pode não estar presente.
- I10 save/load e seed preservam relatórios, timers e decisões posteriores.

## 9. Implementação incremental e aberturas

I0 IntelStore e FactionView do anexo 01; I1 sensores simples e observações;
I2 fusão, atraso e incerteza; I3 sensores táticos. Pendências: LOS realista,
contramedidas eletrônicas, pistas falsas, identificação amigo-inimigo, limiares
por sensor, cobertura meteorológica e erro posicional dependente do terreno.
