# Controle territorial, inteligência e condições de término da guerra

ANEXO TÉCNICO 01

Especificação implementável para o núcleo da campanha dinâmica — complemento ao Documento de Concepção v0.1.

Estado: proposta técnica de referência. Decisões consolidadas: setores capturáveis, coloração WEST/EAST, reconhecimento imperfeito, campanha autônoma, controle por forças terrestres e término explícito da guerra. Valores numéricos deste anexo são padrões iniciais configuráveis, sujeitos a testes de simulação.

## 1. Objetivo, escopo e invariantes

Este anexo define o modelo autoritativo de setores, controle e disputa territorial; as visões de inteligência por facção; regras de captura e de exibição no mapa 2D; e uma máquina de estados para encerrar campanhas. A implementação deve funcionar em modo headless, sem raylib, gráficos 3D, cockpit ou física de aeronaves.

| **Obrigatório na primeira implementação**                                              | **Fora do escopo inicial**                                                        |
|----------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------|
| Setores em grade, vizinhança, infraestrutura, domínio, disputa e histórico.            | Fronteiras vetoriais suaves, polígonos GIS complexos e simulação de cada soldado. |
| Observações, confiança, deterioração temporal e mapa por facção.                       | Radar e linha de visada fisicamente precisos; sensores completos de helicópteros. |
| Conquista terrestre, retirada, neutralidade, suprimentos e transições determinísticas. | Controle de tanque pelo jogador e combate balístico detalhado.                    |
| Vitória por capitulação/objetivos, impasse opcional, encerramento idempotente.         | Diplomacia aberta, ocupação civil complexa e política interna simulada.           |

- INVARIANTE A — há um único estado real do mundo. Cada facção possui uma visão separada, nunca um segundo “mundo real”.

- INVARIANTE B — nenhuma unidade aérea sozinha conquista ou mantém um setor; ações aéreas podem apoiar o controle terrestre.

- INVARIANTE C — a cor no mapa expressa o controle conhecido/estimado da facção observadora; dados reais somente em depuração ou observador autorizado.

- INVARIANTE D — o mesmo seed, configuração, eventos e comandos ordenados devem reproduzir os mesmos resultados.

## 2. Modelo espacial e identificadores

### 2.1 Grade dos setores

Usar grade quadrada regular, com coordenadas inteiras (x,y) e IDs estáveis derivados da posição: id = y\*width+x. Padrão inicial: setores de 2 km de lado; teatro protótipo 16×16 km = 8×8 setores. Geometria e extensão são parâmetros, não constantes de código. Setores de borda podem conter parcialmente terra/água. Para a v0, toda área jogável é associada a um setor.

Adjacência terrestre padrão: quatro vizinhos ortogonais (N/S/L/O). Diagonais não são conexões de avanço/captura; ligações por estradas, pontes e travessias são registradas no grafo de transporte, independente da grade. A simulação de movimentação operacional deve consultar terreno e conectividade, não assumir que toda fronteira é transitável.

### 2.2 Entidades e propriedades

| **Entidade**  | **Campos mínimos**                                                                                                                    |
|---------------|---------------------------------------------------------------------------------------------------------------------------------------|
| Sector        | id, x, y, terrain_type, traversable, objective_points, installation_ids, controller, control_state, contested_since, last_change_tick |
| Faction       | id (WEST/EAST), headquarters_ids, active_units, supplies, command_state, war_status                                                   |
| GroundUnit    | id, faction_id, sector_id, strength, cohesion, morale, readiness, posture, supply_state, combat_capable, orders                       |
| Installation  | id, type, sector_id, operator_faction, integrity, status, garrison_required, repair_work_remaining                                    |
| TransportEdge | from_node, to_node, kind, capacity, travel_time, passable, risk                                                                       |
| CampaignEvent | tick, sequence, type, actor_id, target_id, payload                                                                                    |

Representar controllers por enum: NEUTRAL, WEST, EAST. Separar o proprietário político/histórico de uma instalação (se necessário) de seu operador atual. Não usar simplesmente “última unidade que passou” como controlador.

## 3. Controle real, contestação e captura

### 3.1 Máquina de estados do setor

| **control_state** | **Definição**                                                         | **Exibição no mapa real**                         |
|-------------------|-----------------------------------------------------------------------|---------------------------------------------------|
| SECURE            | Uma facção controla e possui presença ou capacidade de sustentação.   | Preenchimento sólido da facção.                   |
| CONTESTED         | Existe oposição terrestre efetiva ou um processo de captura em curso. | Cor-base + hachuras/contorno de disputa.          |
| NEUTRAL           | Nenhuma facção controla de fato o setor.                              | Cinza/sem preenchimento.                          |
| UNSTABLE          | Controle recém-estabelecido, ainda não consolidado.                   | Cor da facção + padrão discreto de instabilidade. |

controller indica quem administra o setor mesmo quando CONTESTED; não deve ser automaticamente NEUTRAL quando o inimigo inicia ataque. UNSTABLE é estado temporário após mudança de controle. Um setor pode estar SECURE sem tropa permanente somente se houver capacidade de sustentação definida (ex.: guarnição local abstrata); a v0 pode exigir guarnição terrestre persistente em objetivos estratégicos.

### 3.2 Regras de disputa e conquista

1.  Determinar forças terrestres combatentes presentes por facção; excluir aeronaves, unidades destruídas, em retirada completa ou incapazes de ocupar terreno.

2.  Se houver defensores e atacantes combatentes no mesmo setor, marcar CONTESTED, conservar o controlador e disparar/continuar resolução operacional de combate.

3.  Se oposição eficaz cessar, verificar requisito de ocupação: força terrestre atacante apta, ordem compatível e setor acessível; iniciar tempo de consolidação, sem troca instantânea.

4.  Após período contínuo de ocupação (padrão: 30 minutos simulados para setor comum; 120 minutos para objetivo estratégico), transferir controller e marcar UNSTABLE por um período configurável.

5.  Se a unidade ocupante sair antes de completar a ocupação, cancelar ou suspender progresso conforme regra fixa (v0: cancelar). Se forças opositoras reaparecerem, interromper ocupação e retornar a CONTESTED.

6.  Instalações estratégicas não mudam sua operabilidade imediatamente: aplicar evento de captura, danos existentes, segurança, guarnição e tempo de reativação independentes.

A vitória tática em combate, por si só, não muda cor: é preciso ocupação terrestre. Unidades de reconhecimento podem atravessar território sem anexá-lo. Uma força de helicópteros pode destruir uma defesa ou bloquear a logística, facilitando a conquista posterior.

### 3.3 Sustentação e isolamento

Setor conquistado precisa poder receber ordens e suprimentos. Na v0, isolamento não gera perda instantânea do controle; reduz prontidão da guarnição, enfraquece sustentação e cria risco operacional. Uma queda por ausência total de sustentação só ocorre por regra explícita de abandono (ex.: guarnição saiu e não existe defesa local); não por um cálculo oculto de “influência”.

### 3.4 Infraestrutura e objetivos

Cada instalação ocupa um setor, mas sua conquista e sua capacidade funcional são estados distintos. Exemplos: refinaria tomada porém danificada; radar capturado sem energia; base aérea dominada sem combustível ou equipes disponíveis. Pontos de vitória pertencem ao objetivo, não ao mero fato de o setor possuir área grande.

## 4. Frente de batalha e domínio do mapa

O preenchimento azul/vermelho deriva do controller por setor. Não armazenar uma segunda malha de “influência visual”. A linha de frente é calculada onde dois setores ortogonalmente adjacentes pertencem a facções opostas; setores CONTESTED recebem tratamento especial. O desenho pode suavizar contornos apenas visualmente, sem modificar o estado.

| **Informação**             | **Regra**                                                                                                                      |
|----------------------------|--------------------------------------------------------------------------------------------------------------------------------|
| Mapa real (debug)          | WEST azul, EAST vermelho, NEUTRAL cinza, CONTESTED hachurado, UNSTABLE contorno pontilhado.                                    |
| Fronteira                  | Arestas entre setores controlados por facções diferentes; áreas neutras sem linha WEST/EAST direta.                            |
| Setores sem reconhecimento | Usar informação conhecida da facção com padrão de incerteza; não revelar controller real.                                      |
| Conquista                  | Animação e atualização apenas após evento de mudança de controller confirmado na visão correspondente.                         |
| Estatística de controle    | Contagem de setores e, separadamente, pontos estratégicos ponderados. Métricas de debug são reais; no HUD comum são estimadas. |

A representação visual por grade pode ser de baixa resolução e sobreposta a terreno, estradas, rios, cidades e instalações. O jogador poderá alternar camadas: território, frente, inteligência, logística, missões, defesa aérea conhecida e objetivos.

## 5. Reconhecimento e conhecimento por facção

### 5.1 Separação entre verdade e observações

Para cada facção manter FactionIntelStore contendo SectorIntel e ContactIntel. Nenhum agente da IA estratégica deve ler controller, unidades e instalações inimigas diretamente do WorldState; só consultas filtradas à própria visão e a eventos públicos autorizados.

| **Registro** | **Campos mínimos**                                                                                                            |
|--------------|-------------------------------------------------------------------------------------------------------------------------------|
| SectorIntel  | sector_id, believed_controller, control_confidence, observed_at, source, dispute_suspected, known_installations, known_access |
| ContactIntel | contact_id, classification, estimated_sector, estimated_strength, confidence, first_seen, last_seen, source, status           |
| IntelEvent   | tick, faction_id, sector_id/contact_id, kind, source_id, confidence_delta, payload                                            |

A posição conhecida de um contato é uma estimativa; não é um ponteiro público para a entidade real. Internamente, para rastrear e mesclar observações, o simulador pode associar um ID real de maneira privada, mas a API de facção não o expõe.

### 5.2 Fontes de observação

| **Fonte**                      | **Capacidade simplificada v0**                                                                                            |
|--------------------------------|---------------------------------------------------------------------------------------------------------------------------|
| Guarnição/força terrestre      | Observa o próprio setor e vizinhos transitáveis segundo alcance configurado e condições de terreno.                       |
| Patrulha/voo de reconhecimento | Observa os setores sobrevoados/percorridos e um raio configurável; não conquista terreno.                                 |
| Radar e posto de observação    | Produz contatos compatíveis com o sensor e sua cobertura; não revela automaticamente controle terrestre.                  |
| Informação compartilhada       | Propaga relatos aliados com atraso e confiança apropriada.                                                                |
| Mudanças evidentes             | Ataques sofridos, ocupação de instalação própria ou perda de ligação logística geram pistas, não conhecimento onisciente. |

Na v0, a visibilidade pode ser calculada por alcance em setores, terreno de bloqueio simplificado e eventos de presença; sensores específicos serão refinados depois. O observador só identifica efetivamente aquilo que a regra permite; estar dentro de um raio não implica certeza total.

### 5.3 Decadência da informação

Manter último valor observado e hora; reduzir confiança com a idade. Proposta inicial: confidence(t) = confidence_at_observation × 2^(-elapsed_minutes / half_life_minutes). Valores são configurados por classe: controle territorial 360 min; unidade terrestre móvel 30 min; instalação fixa 1440 min. A função diminui confiabilidade, não altera retroativamente a verdade nem faz unidades “sumirem” da realidade.

| **Confiança atual** | **Representação ao jogador**                                         | **Regra de uso pela IA**                                           |
|---------------------|----------------------------------------------------------------------|--------------------------------------------------------------------|
| ≥ 0,75              | Controle ou contato com preenchimento definido e hora da informação. | Pode apoiar planejamento, com avaliação de risco.                  |
| 0,35–0,75           | Hachura/transparência; símbolo de informação antiga.                 | Tratar como estimativa; buscar confirmação para missões sensíveis. |
| \< 0,35             | Desconhecido ou último relato esmaecido, conforme opção do mapa.     | Não tratar como fato; sugerir reconhecimento.                      |

Inicialização recomendada: cada facção conhece com alta confiança seus setores, suas próprias instalações e suas unidades. Áreas inimigas ou neutras fora do reconhecimento começam desconhecidas, exceto fatos públicos explicitamente fornecidos pelo cenário. Reobservações atualizam crenças e carimbo de tempo; relatos contraditórios devem preservar histórico e escolher por evidência e recência, não simplesmente sobrescrever cegamente.

### 5.4 Captura, boatos e erro de avaliação

Após uma captura real, somente o lado que ocupa o setor recebe observação direta imediata; o inimigo permanece com informação anterior até obter indícios ou reconhecimento. Mensagens de “setor perdido” podem chegar por comunicações de unidades em retirada, segundo regras de latência da rede. A interface pode exibir “controle estimado”, “contestado”, “desconhecido” e “última confirmação”.

Não alterar a crença só porque controller mudou no WorldState: toda atualização de inteligência deve ter uma fonte de observação ou evento informativo explicitamente modelado.

## 6. IA estratégica e interação do jogador

Planejadores operacionais recebem um snapshot de IntelStore de sua facção, seus próprios estoques e unidades, e objetivos conhecidos. Eles devem distinguir “inimigo confirmado”, “possível presença inimiga” e “região desconhecida”. A confiança altera riscos, prioridades e a necessidade de reconhecimento. A IA não recebe coordenadas secretas dos alvos.

No mapa 2D, o jogador pode observar frente estimada, selecionar setor, consultar instalações conhecidas, atribuir reconhecimento, propor ataque/defesa, definir prioridade, escolher unidades aptas e interferir nas missões da sua facção conforme autoridade configurada.

| **Comando**        | **Validações mínimas**                                                                                      |
|--------------------|-------------------------------------------------------------------------------------------------------------|
| Reconhecer setor   | Unidade apta, alcance, combustível, rota, risco conhecido; gerar missão e relatório ao retorno/transmissão. |
| Atacar ou ocupar   | Setor-alvo válido, força terrestre disponível para ocupação, suprimentos, rota, ordens e inteligência.      |
| Defender/guarnecer | Capacidade da unidade, deslocamento, disponibilidade e conflito de tarefas.                                 |
| Alterar prioridade | Permissão do jogador, disponibilidade e orçamento logístico.                                                |
| Cancelar operação  | Estado da missão e consequências de ordem já em execução.                                                   |

Ordenar reconhecimento de área desconhecida é sempre permitido se houver capacidade operacional. Atacar contato desconhecido não deve gerar “teletransporte” para posição verdadeira: missão deve usar a última localização estimada e poder falhar por ausência de alvo.

## 7. Fluxo de atualização determinístico

O clock é independente do renderizador. Ordem proposta por tick estratégico, executada em sequência estável por ID:

1\. apply_queued_commands(tick)

2\. resolve_movement_and_operational_combat(tick)

3\. update_ground_presence_and_capture_progress(tick)

4\. update_installations_and_supply_links(tick)

5\. generate_sensor_observations(tick)

6\. update_each_faction_intel(tick)

7\. run_operational_planners_if_due(tick)

8\. evaluate_war_end_conditions(tick)

9\. append_events_and_snapshot_if_due(tick)

Tick estratégico padrão: 60 segundos simulados; missões/combates podem ter sub-ticks determinísticos. Não usar tempo de parede para o avanço da guerra. Eventos ocorridos em um tick são ordenados por (tick, phase, sequence, entity_id). Leituras de facção passam por um facade de inteligência; relatórios em modo observador/debug são explicitamente separados.

## 8. Condições de fim da guerra

### 8.1 Estados da campanha

| **Estado**                  | **Significado**                                                                         |
|-----------------------------|-----------------------------------------------------------------------------------------|
| ONGOING                     | Guerra em curso; ambos os lados podem agir.                                             |
| CEASEFIRE_PENDING           | Condição de capitulação ou acordo reconhecida; temporizador de confirmação em execução. |
| WEST_VICTORY / EAST_VICTORY | Resultado final, ordens militares ofensivas encerradas.                                 |
| DRAW                        | Encerramento sem vencedor, somente pelas regras explícitas do cenário.                  |
| ABORTED                     | Interrupção administrativa/manual, não é resultado militar.                             |

O fim da guerra deve ser resolvido exclusivamente pelo motor estratégico. Uma mudança de cor isolada não encerra automaticamente a campanha. A condição final é persistida uma única vez, com causa, tick e estatísticas de encerramento.

### 8.2 Vitória por capitulação — padrão recomendado

Cada facção possui um conjunto configurável de objetivos críticos (ex.: centro de comando principal, capital fictícia, base logística central). A facção pode capitular se TODAS as seguintes condições permanecerem verdadeiras por uma janela contínua de 6 horas simuladas:

7.  Perda de capacidade de comando: nenhum centro de comando crítico próprio operacional sob seu controle, OU um evento formal de rendição emitido pelo cenário/comandante.

8.  Perda de capacidade de resistência: índice de forças organizadas disponíveis abaixo de 20% da força de referência inicial, OU perda de pelo menos 75% dos pontos estratégicos críticos.

9.  Sem perspectiva imediata de recuperação: ausência de reforço confirmado capaz de restaurar comando ou força no horizonte configurado (padrão: 24 h simuladas).

Os critérios 20%, 75%, 6 h e 24 h são parâmetros propostos, não verdades históricas. Para evitar travamento, o cenário também pode permitir rendição voluntária gerada pela IA quando custo de continuidade, moral e capacidade logística forem muito desfavoráveis; deve ser evento explícito e auditável.

### 8.3 Vitória por objetivo do cenário

Cada campanha define VictoryObjective com: id, owner_faction, objective_type, sector_ids, condition, required_hold_minutes, weight, enabled. Uma missão/campanha pode exigir manter um conjunto de bases, controlar determinado corredor ou preservar instalações até um prazo. A vitória ocorre apenas se o cenário habilitar essa condição. Padrão para primeira campanha: capitulação + objetivos críticos; não vitória por porcentagem bruta de território.

### 8.4 Impasse e empate

Não declarar empate simplesmente porque a linha de frente não se move por algumas horas. Implementar uma saída opcional para cenários de duração finita: ao atingir max_campaign_days (padrão recomendado de teste: 30 dias simulados), se não houve vitória, resultado DRAW com causa TIME_LIMIT. Campanhas sem limite podem permanecer ONGOING indefinidamente, inclusive com frentes estabilizadas.

Uma futura regra de cessar-fogo negociado pode considerar exaustão e ausência de progresso, mas não integra a v0. Se ambas capitularem no mesmo tick, aplicar resolução explícita de empate simultâneo, não depender da ordem WEST/EAST.

### 8.5 Prioridade de avaliação e transição

if war_state is terminal: return

if explicit_surrender(WEST) and explicit_surrender(EAST): finish(DRAW, MUTUAL_SURRENDER)

else if capitulation_confirmed(WEST): finish(EAST_VICTORY, CAPITULATION)

else if capitulation_confirmed(EAST): finish(WEST_VICTORY, CAPITULATION)

else if scenario_objective_win(WEST) and scenario_objective_win(EAST): finish(DRAW, SIMULTANEOUS_OBJECTIVE)

else if scenario_objective_win(WEST): finish(WEST_VICTORY, SCENARIO_OBJECTIVE)

else if scenario_objective_win(EAST): finish(EAST_VICTORY, SCENARIO_OBJECTIVE)

else if time_limit_reached: finish(DRAW, TIME_LIMIT)

A implementação também deve resolver capitulações simultâneas sem rendição explícita; o princípio é simetria determinística. Durante CEASEFIRE_PENDING, o prazo reinicia ou é cancelado se a condição deixa de valer. Após estado terminal, a campanha pode ser inspecionada e reproduzida, mas não avança nem emite novos eventos de batalha.

## 9. Configuração e persistência

Preferir parâmetros em arquivo de cenário, com versão de schema e defaults seguros. Exemplo textual orientativo (não depende de linguagem específica de configuração):

world: {width: 8, height: 8, sector_size_m: 2000}

tick_seconds: 60

capture: {normal_hold_minutes: 30, critical_hold_minutes: 120}

intel: {territory_half_life_min: 360, mobile_half_life_min: 30,

installation_half_life_min: 1440, uncertain_below: 0.75,

unknown_below: 0.35}

victory: {mode: capitulation_and_objectives, confirm_minutes: 360,

force_threshold: 0.20, critical_points_lost: 0.75,

reinforcement_horizon_minutes: 1440, max_campaign_days: 30}

Salvar: versão do schema, seed/estado do RNG, relógio, grid e instalações, unidades e ordens, status de captura por setor, stores de inteligência de ambas as facções, reforços em trânsito, parâmetros, máquina de estados da guerra e journal de eventos. Carregar e salvar não pode alterar decisões futuras; commits de snapshot devem ocorrer em fronteiras de tick.

## 10. API mínima sugerida (sem dependência gráfica)

WorldState get_world_snapshot_for_debug() // acesso restrito

Sector get_sector_truth(SectorId id) // somente engine

FactionView get_faction_view(FactionId faction, Tick t) // API normal

IssueResult issue_order(FactionId actor, Order request)

void advance_ticks(uint32_t count)

WarResult get_war_status()

EventList events_since(EventSequence sequence)

FactionView é um DTO construído a partir de dados próprios e IntelStore. Não incluir coleções de unidades inimigas reais nem posições verdadeiras ocultas. O mapa 2D utiliza apenas FactionView; painel de depuração/debug requer uma capacidade separada. Usar tipos explícitos para SectorId, FactionId, Tick e entidade para evitar IDs misturados.

## 11. Testes automatizados e critérios de aceite

| **ID** | **Cenário de teste**                                             | **Resultado esperado**                                                          |
|--------|------------------------------------------------------------------|---------------------------------------------------------------------------------|
| T01    | Somente helicóptero WEST entra em setor EAST.                    | Controller permanece EAST; nenhuma captura.                                     |
| T02    | Unidade terrestre WEST expulsa defesa EAST e ocupa setor 30 min. | Captura após prazo, evento único e estado UNSTABLE.                             |
| T03    | Atacante sai após 20 min e retorna.                              | Contagem reinicia; sem captura instantânea.                                     |
| T04    | Duas facções terrestres combatentes no mesmo setor.              | CONTESTED; controlador permanece até resolução/ocupação.                        |
| T05    | Refinaria muda de controle, mas está danificada.                 | Novo operador registrado; produção não reinicia magicamente.                    |
| T06    | WEST captura setor sem observador EAST.                          | EAST mantém crença anterior; verdade muda apenas internamente.                  |
| T07    | Contato móvel fica 60 min sem nova observação.                   | Confiança cai pela função, conforme half-life configurada.                      |
| T08    | Reconhecimento revisita setor.                                   | Intel da facção observadora atualiza; adversária não recebe atualização grátis. |
| T09    | IA tenta planejar ataque a alvo fora de sua inteligência.        | Não recebe posição verdadeira; pode solicitar reconhecimento.                   |
| T10    | Perda de 90% dos setores sem capitulação/objetivo habilitado.    | Guerra continua; contagem territorial isolada não vence.                        |
| T11    | Condições de capitulação presentes por 5 h e revertidas.         | Temporizador cancelado, sem vitória.                                            |
| T12    | Condições de capitulação presentes por 6 h completas.            | Vitória adversária registrada uma única vez.                                    |
| T13    | Ambos satisfazem capitulação no mesmo tick.                      | Empate simultâneo determinado simetricamente.                                   |
| T14    | Limite de 30 dias sem vencedor.                                  | DRAW/TIME_LIMIT se limite ativado.                                              |
| T15    | Salvar/recarregar no meio da ocupação.                           | Mesmo tempo remanescente e desfecho da execução contínua.                       |
| T16    | Mesma seed e mesmos comandos, duas execuções headless.           | Mesmo hash do estado final e sequência de eventos.                              |
| T17    | Isolamento logístico de guarnição.                               | Deterioração gradual, sem troca inexplicável de controlador.                    |
| T18    | Frente derivada dos setores após captura.                        | Arestas fronteiriças recalculadas; render não altera controle.                  |

## 12. Plano incremental para o Codex

10. M1 — World grid: IDs, setores, vizinhos, instalações e fixtures, sem IA, sem gráficos. Entrega: testes de topologia e serialização.

11. M2 — Controle territorial: estados, combate operacional de teste com eventos injetados, ocupação temporizada e frente derivada. Entrega: testes T01–T05, T17–T18.

12. M3 — Inteligência: stores por facção, observadores, decadência, views protegidas e missões de reconhecimento abstratas. Entrega: testes T06–T09; auditoria para acesso indevido ao truth.

13. M4 — Condições de término: objetivos críticos, capitulação, timers e empate, com persistência. Entrega: testes T10–T16.

14. M5 — Interface 2D: camadas coloridas, hachuras de contestação/incerteza, última observação, ordens e legendas, ligada exclusivamente a FactionView. Entrega: teste visual, sem introduzir dependência da interface no core.

Antes de cada marco: testes headless; após: execução longa com seed fixa e métricas por tick (setores controlados, eventos de captura, observações, ordens, resultado de guerra, custo por tick). Não adicionar otimizações complexas antes de medir gargalos.

## 13. Critérios de qualidade e decisões abertas

- Separação rigorosa de responsabilidades: território não decide combate; combate emite resultados; captura resolve ocupação; inteligência publica observações; render somente apresenta.

- O relógio simulado controla ações, manutenção, reconhecimento e vitória. Nada depende de FPS ou da velocidade do computador.

- Todos os limiares, cores e tempos são configuráveis por cenário; defaults não ficam espalhados em código.

- Eventos essenciais: CAPTURE_STARTED, CAPTURE_INTERRUPTED, SECTOR_CONTESTED, SECTOR_CAPTURED, INTEL_OBSERVED, INTEL_UPDATED, WAR_END_PENDING, WAR_ENDED.

- Sem atualização clandestina de conhecimento. É aceitável produzir observações simplificadas na v0, nunca conectar IA e UI diretamente ao estado real.

Decisões ainda abertas, sem bloquear a v0: tamanho final do teatro; métodos de reconhecimento mais detalhados; impacto de condições meteorológicas e comunicações; política de abandono de setores não estratégicos; critérios de negociação de paz; modo sandbox sem prazo; topologia de rotas e pontes; comandante e autoridade do jogador.

## 14. Definição de pronto (DoD)

A funcionalidade está pronta quando: (a) duas facções conseguem disputar e conquistar setores num mapa 2D; (b) frentes mudam exclusivamente em consequência de eventos reais de ocupação; (c) cada facção vê um mapa diferente quando faltam informações; (d) reconhecimento atualiza apenas a visão pertinente; (e) a IA planeja sem onisciência; (f) condições explícitas encerram a guerra e geram relatório; (g) replay e save/load reproduzem resultados; (h) tudo funciona sem criar uma janela gráfica.

Referência principal: “Simulador de helicópteros arcade com campanha dinâmica — Documento de Concepção, versão 0.1”. Este anexo é uma proposta normativa para a primeira implementação e pode ser versionado independentemente.
