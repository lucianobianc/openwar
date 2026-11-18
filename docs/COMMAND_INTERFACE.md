# OpenWar — Operational Command Interface Specification

**Status:** especificação funcional e arquitetural para implementação incremental  
**Revisão:** 1.0 — 18 de novembro de 2026  
**Escopo:** interface estratégica 2D, comando humano, ordens, planejamento de missões e reutilização no futuro jogo de helicópteros  
**Padrão de simbologia adotado:** **NATO APP-6, NATO Joint Military Symbology, edição E, versão 2, promulgada em 16/10/2025, sob STANAG 2019**  
**Documentos relacionados:** `DESIGN.md`, `OPERATIONS.md`, `INTELLIGENCE.md`, `TERRITORY.md`, `MILITARY_FORCES.md`, `LOGISTICS.md`, `SCENARIOS.md`, `CAMPAIGN_SETUP.md`, `TACTICAL.md` e `GEOGRAPHIC_WORLD.md`.

> **Contrato principal:** a interface é um cliente de comando, planejamento e observação do motor de campanha — nunca uma segunda simulação. A mesma base de mapa, símbolos e editor de missões serve ao OpenWar standalone e ao futuro jogo de helicópteros; somente contexto, autoridade e executor tático diferem.

## 1. Finalidade e produtos atendidos

O OpenWar é um motor independente de campanha militar dinâmica. Sua interface operacional deve funcionar como **aplicação standalone**, permitindo que um operador acompanhe e interfira na guerra de uma facção, e como **sistema de briefing/planejamento** do produto posterior em que o jogador pilotará um helicóptero.

### 1.1 Dois casos de uso, uma interface

| Capacidade | OpenWar standalone | Futuro jogo de helicópteros |
|---|---|---|
| Mapa 2D, pan/zoom, camadas | Integral | Integral, com permissões |
| Inteligência e relatórios | Conforme observador/facção | Conforme facção e papel |
| Seleção de unidades e instalações | Integral | Conforme autoridade |
| Ordens terrestres, logísticas e aéreas | Conforme autoridade | Conforme autoridade, podendo ser reduzida |
| Criar, editar e acompanhar missões | Sim | Sim |
| Planejar waypoints, horários e alvos | Sim | Sim |
| Escolher aeronave e preparar missão | Sim, quando aplicável | Sim |
| Executar missão aérea | IA/abstração da campanha | IA ou jogador no simulador tático |
| Debriefing e atualização da guerra | Sim | Sim, pelo mesmo estado |
| Objetos 3D e física de voo | Não exigidos | Responsabilidade do outro produto |

A interface standalone não deve exibir botões fictícios de voo. A interface do jogo pode acrescentar `Assumir aeronave`, sem duplicar editores de rotas, unidades, briefings ou ordens.

### 1.2 Requisitos normativos

- **UI-001:** motor da campanha executa sem UI e sem dependência de renderização.
- **UI-002:** mapa 2D scrollável, zoom contínuo/quantizado e símbolos em posições geográficas reais ou **estimadas**, conforme a visão autorizada.
- **UI-003:** facção, papel e autoridade determinam o que pode ser visto e ordenado; a interface não revela informação oculta.
- **UI-004:** ordens e missões são validadas e efetivadas pelo núcleo, com resposta de aceitação/rejeição e motivos, nunca por edição direta do estado.
- **UI-005:** emissão de ordem reserva recursos somente no commit aceito, de maneira atômica e idempotente.
- **UI-006:** AI e operador submetem intenções à mesma lógica de missão e restrições militares/logísticas.
- **UI-007:** uma missão de helicóptero mantém identidade única antes, durante e depois de eventual execução tática pilotável.
- **UI-008:** apresentação gráfica, câmera e navegação não alteram relógio simulado, RNG, eventos ou determinismo.
- **UI-009:** simbologia militar baseada explicitamente em **NATO APP-6(E), versão 2**, e não em convenções improvisadas como padrão principal.
- **UI-010:** ambiguidades de edição, licença ou cobertura de símbolos são documentadas; não declarar conformidade APP-6 completa sem testes contra a referência normativa.
- **UI-011:** alterações de regras ou formato exigem versionamento e preservação de saves/campanhas nos termos do M8.
- **UI-012:** frontend standalone e integração com o jogo compartilham modelos de leitura e comandos; não existe uma verdade paralela da interface.

## 2. Arquitetura e separação de responsabilidades

```text
                          OpenWar Campaign Core
       World | Economy | Logistics | Forces | Intelligence | Strategy
                                 |
                    Read snapshots / Issue commands
                                 |
                      Command Interface API
                     /                       \
            Standalone OpenWar          Helicopter game shell
                     \                       /
             Shared 2D map + operational UI + mission editor
                                                  |
                                    Tactical mission adapter
                                                  |
                                      Helicopter simulator
```

A `Command Interface API` pode ser, inicialmente, uma interface C++ no mesmo processo. Não introduzir HTTP, WebSocket, RPC ou microserviços apenas para cumprir separação de camadas. O frontend não precisa conhecer tipos internos mutáveis de economia/combate.

### 2.1 Papéis da interface

| Componente | Responsabilidade | Não deve fazer |
|---|---|---|
| `CampaignQuery`/snapshot | Estado do mundo visível ao usuário, com tempo e revisão | Expor verdade oculta a facção |
| `CommandGateway` | Receber, validar e confirmar ordens, retornando IDs e erros | Resolver combates ou alterar recursos fora do núcleo |
| `MapView` | Desenhar geografia, símbolos, sobreposições e câmera | Ser responsável pelo movimento de unidade |
| `SelectionModel` | Seleção, destaque, tooltip, grupo de seleção | Alterar entidade só porque foi selecionada |
| `MissionDraft` | Construção local de plano não comprometido | Reservar combustível/equipamento antes da confirmação |
| `MissionEditor` | Validar rascunho, solicitar cotação, confirmar e acompanhar | Reimplementar regras de alcance/estoque |
| `TacticalAdapter` | Traduzir missão confirmada em contexto pilotável e devolver eventos | Manter outro `CampaignState` autoritativo |

### 2.2 Contratos conceituais mínimos

```text
OperatorContext:
    campaign_id, faction (optional), role, authority, time_visibility

MapSnapshot:
    tick, snapshot_revision, visible_terrain, known_control,
    own_units, observed_contacts, known_installations,
    known_missions, confidence, labels, warnings

CommandRequest:
    actor_id, authority_context, request_id, expected_revision,
    operation_kind, target, assigned_entities, parameters

CommandResult:
    accepted, command_id?, mission_id?, effective_tick?,
    rejection_code?, human_readable_reason?, updated_revision?

MissionDraft:
    faction, kind, participants, source, target, waypoints,
    planned_window, route_constraints, payload/loadout,
    abort_policy, briefing_notes
```

Os nomes acima expressam **contratos**, não impõem novas classes C++ se as APIs existentes já os satisfizerem. Uma visão `FactionView` atual pode ser adaptada para `MapSnapshot` sem copiá-la inteira ou expor estruturas mutáveis.

## 3. Papéis, facções e acesso à informação

### 3.1 Papéis suportados

- **Observador**: acompanha a campanha; pode receber visão integral **somente em modo explicitamente autorizado**. Nunca emite ordens.
- **Comandante de facção**: vê as próprias forças, estoques autorizados e relatórios sobre o inimigo; cria ordens estratégicas/operacionais dentro dos limites definidos.
- **Comandante operacional / esquadrão**: controla um subconjunto de formações e tipos de missão, sem acesso irrestrito a toda a facção.
- **Piloto / planejador de missão**: pode selecionar missões e aeronaves permitidas, planejar e executar voos; não recebe poderes gerais de comando por ser o jogador.

`WEST`/`EAST` continuam sendo facções do cenário. **Afiliação APP-6** (amigo, hostil, neutro, desconhecido) é uma classificação **relativa ao observador e ao conhecimento disponível**, não uma conversão fixa de WEST em azul e EAST em vermelho. Um contato não identificado não pode virar hostil por saber sua facção verdadeira no núcleo.

### 3.2 Separação de conhecimento

O mapa tático-operacional mostra contatos adversários como **estimativas** com `observed_tick`, confiança, classificação e precisão espacial disponíveis. Posições estimadas podem ser desenhadas com halo/área de incerteza; posições exatas não são reveladas ao selecionar, aproximar o zoom ou inspecionar uma rota. Informação antiga permanece marcada como antiga ou desaparece sob regra do subsistema de inteligência. Não mostrar dados internos de inimigos (combustível, ordem atual, inventário exato) sem fonte observacional explícita.

A renderização, o hit-testing, as pesquisas e as sugestões de rota devem consumir a **mesma visão filtrada**. Evitar o vazamento indireto por seleção de objetos invisíveis, tooltip, nome de arquivo, ordem de renderização, minimapa ou número de símbolos ocultos.

## 4. Organização funcional da tela

**Layout de referência**, ajustável a diferentes resoluções:

```text
+------------------------------------------------------------------+
| Campaign / Faction / Clock / Pause-Run-Speed / Save / Reports     |
+------------+------------------------------------+----------------+
| Filters    |                                    | Selection      |
| Layers     |       GEOGRAPHIC OPERATIONAL MAP   | Properties     |
| Units      |       roads, rivers, relief        | Readiness      |
| Missions   |       APP-6 symbols, routes        | Orders         |
| Intel      |       objectives, annotations      | Logistics      |
| Logistics  |                                    | Create mission |
+------------+------------------------------------+----------------+
| Event timeline / mission status / warnings / messages             |
+------------------------------------------------------------------+
```

O mapa é o elemento central. Barras laterais devem poder recolher para aproveitar monitores pequenos; janelas de detalhe podem ser móveis. Nada exige uma interface complexa de janelas internas na primeira implementação.

### 4.1 Mapa

- Pan por arrasto e/ou teclado; zoom sobre posição do cursor; comandos `enquadrar seleção`, `enquadrar objetivo`, `voltar à vista anterior`.
- Norte identificado; escala gráfica e coordenadas do cursor; distância medida em metros/km; orientação e projeção coerentes.
- Símbolos ancorados nas **posições conhecidas** das formações, nunca no centro do setor por convenção visual obrigatória.
- Geografia contínua: estradas e rios atravessam fronteiras da malha; florestas e relevo não são quadrados da grade.
- Overlays opcionais: controle territorial, setores, estradas, rios, relevo, cobertura, frentes, objetivos, rotas, logística, infraestrutura, ameaça **conhecida**, inteligência.
- Legenda de overlays, unidades e cores; preferências locais independentes de save da campanha.
- Seleção em regiões densas por lista/disambiguador; agrupamento visual (*decluttering*) no zoom distante, preservando identidade e quantidade de unidades subjacentes.

### 4.2 Navegação e comandos

| Ação | Interação inicial sugerida |
|---|---|
| Navegar no mapa | Arrastar ou setas/WASD, configurável e sem interferir com editor ativo |
| Zoom | Roda do mouse e atalhos `+`/`-` |
| Selecionar | Clique esquerdo, com destaque persistente |
| Contexto | Clique direito mostra apenas ordens aplicáveis |
| Criar missão | Comando explícito abre rascunho, com unidade/alvo pré-selecionados quando houver |
| Adicionar waypoint | Ferramenta de edição ativa, clique no mapa, arrastar para ajustar |
| Medir distância | Ferramenta não operacional, sem criar ordens |
| Pausar/avançar tempo | Controle com feedback do tick/speed atual |
| Cancelar edição | `Esc` reverte somente o rascunho local, sem cancelar missão confirmada |

Atalhos devem ser configuráveis, com foco de teclado claro. No jogo tático, WASD pode ser usado pela câmera 3D; o mapa 2D não deve depender de bindings globais fixos. Suporte pleno a teclado, mouse, escalonamento de DPI e indicação de foco integra o DoD de interface.

### 4.3 Painéis e relatórios

- **Unidade:** identificador, nome, facção relativa, escalão, localização conhecida, status, composição, prontidão, missão, combustível, munição, manutenção e subordinados, respeitando permissões.
- **Missão:** objetivo, tipo, origem, participantes, rota, fase, recursos reservados, janela, previsão e motivo de impedimento.
- **Instalação:** classe, localização, funcionalidade conhecida, estoques e capacidade quando autorizados.
- **Logística:** depósitos, requisições, transportadores, rotas conhecidas e gargalos, sem inventar cargas invisíveis ao operador.
- **Inteligência:** contatos, confiança, horário da observação, fontes autorizadas e relatórios associados.
- **Histórico:** eventos ordenados e filtráveis por tempo, tipo, facção, local e entidade; nenhum evento de depuração restrito aparece no modo de facção sem autorização.

## 5. Simbologia militar OTAN — contrato normativo

### 5.1 Referência exata

Adotar **APP-6 — NATO Joint Military Symbology**, **Edition E, Version 2**, promulgada em **16 de outubro de 2025**, vinculada ao **STANAG 2019**. A fonte documental oficial do registro é a base ASSIST (Departamento de Defesa dos EUA, índice de documentos da OTAN): https://quicksearch.dla.mil/qsDocDetails.aspx?ident_number=275338; acordo de implementação STANAG 2019: https://quicksearch.dla.mil/qsDocDetails.aspx?ident_number=97629.

A versão E/2 tem **distribuição controlada** no índice consultado. Este documento especifica a adoção do padrão, **não reproduz suas tabelas completas nem afirma acesso público integral ao texto vigente**. Para implementar identificadores de símbolos, combinações válidas, modificadores e posicionamento exato, obter uma cópia autorizada da publicação aplicável e construir testes contra ela. Uma edição aberta mais antiga pode auxiliar o desenvolvimento preliminar, mas **não substitui a edição E/2 na alegação de conformidade**. O padrão norte-americano MIL-STD-2525 possui conceitos relacionados, mas **não deve ser chamado de APP-6 ou confundido com a referência normativa escolhida**.

### 5.2 Composição simbólica

A representação APP-6 não é um catálogo de emojis ou imagens avulsas. Um símbolo militar é **composto** a partir de propriedades semânticas, sujeitas às regras da edição:

- Afiliação/identidade conhecida (amigo, hostil, neutro, desconhecido e demais variantes admitidas).
- Domínio/contexto operacional do elemento (terra, ar, mar etc., conforme conjunto aplicável).
- Natureza ou função do elemento (infantaria, blindados, artilharia, aviação, logística, instalação etc.).
- Estado/condição do símbolo (presente, planejado/antecipado ou outra indicação permitida conforme o tipo de entidade).
- Escalão/tamanho organizacional quando aplicável (equipe, seção, pelotão, companhia, batalhão, regimento, brigada, divisão etc.).
- Modificadores e amplificadores permitidos: designação, unidade superior, quantidade, direção de deslocamento, avaliação, mobilidade e outras informações previstas na norma.
- Ponto de ancoragem geográfica do símbolo, rótulos e posicionamento relativo de campos regulamentados.

**Não confundir** *estado operacional* do OpenWar (`ready`, `repair`, `destroyed` etc.) com o campo de *status gráfico* padronizado da APP-6; parte da prontidão será exibida em painel ou indicador acessório da aplicação, não falsificando um modificador militar oficial.

### 5.3 Identificação visual e acessibilidade

Afiliação deve ser compreensível pela **forma/estrutura do símbolo**, não apenas pela cor. Como referência conceitual amplamente utilizada na família APP-6: enquadramentos de amigo, hostil, neutro e desconhecido empregam formas diferenciadas; o desenho exato, variantes de domínio e exceções devem seguir a edição adotada. Cores convencionais podem apoiar leitura (azul/amigo, vermelho/hostil, verde/neutro, amarelo/desconhecido), mas não são o único canal de informação. Garantir versão monocromática, contraste adequado e legendas.

Na UI: WEST e EAST podem aparecer como amigo ou hostil **conforme a facção visualizadora**; no modo observador, os estilos dos lados devem ser identificáveis e a afiliação relativa deve ser definida em uma política explícita de visualização. Não etiquetar todo contato desconhecido como inimigo por proximidade da frente.

### 5.4 Escalão organizacional não é quantidade de equipamentos

A marca de escalão deve refletir o **nível organizacional real da unidade modelada**, não ser inferida de uma contagem genérica de tanques, helicópteros ou caminhões. Uma formação com três tanques não é automaticamente um pelotão segundo uma doutrina arbitrária; o cenário ou catálogo militar deve declarar a organização. Quando a engine não possuir nível de escalão confiável, exibir o símbolo de função **sem marca de escalão** ou utilizar rótulo de abstração explicitamente documentado; não inventar escalões para preencher o mapa.

Uma aeronave individual e uma esquadrilha também são entidades distintas: símbolo de plataforma/veículo quando apropriado e símbolo de formação quando for uma unidade organizacional. O agrupamento visual por zoom não altera o escalão da unidade.

### 5.5 Cobertura mínima para OpenWar

| Família | Informação mínima prevista para a UI | Observação |
|---|---|---|
| Infantaria / pessoal terrestre | Afiliação, função, escalão se conhecido, posição | Pessoal pode ainda não estar detalhado na engine |
| Blindados | Formação/plataforma, função, escalão, ID | Não confundir tanques com transportadores |
| Veículos e logística | Transporte, apoio, reabastecimento, manutenção | Função conforme catálogo e estado permitido |
| Defesa antiaérea | Categoria funcional, unidade, local conhecido | Não pressupor sensor ou alcance não observado |
| Artilharia | Unidade, função e escalão | Habilitar quando existir no catálogo |
| Aeronaves e helicópteros | Plataforma e/ou unidade aérea, missão e base | Aplicar símbolos do domínio/tipo apropriado |
| Instalações | Aeródromo, depósito, oficina, indústria, posto de comando | Símbolos de instalações conforme padrão quando aplicáveis |
| Contatos e alvos | Identificação observada, estimativa, confiança | A identificação nunca excede a inteligência disponível |
| Controle operacional | Linhas de fase, limites, eixos, objetivos, rotas | Usar **gráficos táticos** padronizados quando houver correspondência; overlays de jogo distinguíveis |

A especificação oficial pode ter diferentes códigos e classes para entidades semelhantes. O catálogo de símbolos do OpenWar deve manter mapeamento explícito por versão, não constantes numéricas inventadas.

### 5.6 Modelo semântico de símbolos

```text
MilitarySymbolModel (conceitual):
    specification: "APP-6(E) v2"
    symbol_identity: código oficial validado para a edição
    affiliation: identidade conhecida para esta visão
    dimension_or_context
    entity_kind / function
    operational_graphic_status
    echelon_if_known
    modifiers: apenas os permitidos pelo código e pela edição
    anchor_point: posição geográfica conhecida/estimada
    estimated_position / timestamp / confidence (metadados OpenWar)
    own_ui_overlays: selected / warning / low_fuel / confidence_area
```

Separar rigorosamente **atributos militares normativos** de **metadados da simulação**. Um anel de seleção, um medidor de combustível ou a transparência de confiança são recursos do OpenWar, sem pretensão de integrarem a APP-6. O código oficial e a função militar são independentes da arte raster/SVG usada para renderizar o símbolo.

### 5.7 Renderer e validação dos símbolos

- Construir uma biblioteca de símbolos vetoriais/SVG ou desenho parametrizado, com IDs estáveis, apta a escalonamento sem perder nitidez.
- Centralizar regra de composição em um único módulo; não espalhar formas e glifos por cada tela.
- Exibir modificação de afiliação conforme o ponto de vista; atualização de identificação por relatório muda o símbolo somente quando a inteligência justificar.
- Aplicar marcação oficial de escalão, estado, função e modificadores quando válidos; não combinar arbitrariamente campos.
- Fornecer legenda e consulta textual do significado de cada símbolo.
- Preparar testes de referência visual (*golden files*) e testes semânticos por edição, em diferentes escalas, cores e modo monocromático.
- Definir conjunto mínimo de símbolos necessário às entidades existentes antes de alegar **conformidade parcial**. Conformidade total só após auditoria de cobertura contra edição autorizada.
- Registrar proveniência/licença de fontes, sprites e bibliotecas de simbologia; padrões publicados e implementações gráficas de terceiros não têm necessariamente a mesma licença.

### 5.8 Generalização visual por zoom

A geometria do mundo continua contínua. Em zoom afastado, a interface pode reduzir rótulos, juntar marcadores próximos em grupos de exibição e destacar unidades de escalão superior; no zoom aproximado, revela símbolos separados e rotas. **Generalização gráfica não consolida unidades reais, não aumenta escalão e não modifica a visão da facção.** Uma agrupação temporária deve indicar quantidade e oferecer expansão/seleção. Proibir que o agrupamento revele unidades adversárias não observadas.

### 5.9 Phase 2 implementation and reference coverage

The normative target remains NATO APP-6 Edition E Version 2 under STANAG 2019.
The DLA ASSIST record identifies this release as controlled distribution.
Phase 2 uses a documented public historical subset pending verification against
the authorized edition. [DLA ASSIST APP-06 record](https://quicksearch.dla.mil/qsDocDetails.aspx?ident_number=275338)

| Rendered family | Public historical reference |
|---|---|
| Friendly, hostile, neutral, and unknown affiliation frames for land units, airspace, and sea surface | National Defence, *NATO Map Symbols (Canadian), Programmed Instruction Package*, January 2000, p. 5 |
| Land infantry, armor, reconnaissance, engineers, artillery, and signals | Same publication, p. 16 |
| Land mechanized infantry | Same publication, p. 23 |
| Land formation echelons from platoon through division | Same publication, pp. 8–9 |
| Administrative and logistics installations with a tab over the frame | Same publication, p. 27 |

[Public copy of the Canadian National Defence instruction package](https://army.ca/wiki/images/8/80/NATO_Map_Symbol_(Canadian)_Programmed_Instruction_Package_Jan_2000.pdf)

The renderer uses screen-sized geometry anchored to each map position. An
unspecified domain uses the generic land-style frame. Function glyphs are
limited to the listed land roles. The white observer frame and W/E labels,
ground transport tag T, report and operational condition tags, selection ring,
and mission pin are OpenWar conventions. Unknown reports carry only the
domain, classification, position estimate, and freshness available through
intelligence. Full edition conformance, SIDCs, standard status variants,
formal label fields, platform and subsurface symbols, and full function and
echelon coverage require further reference validation.

Each formation has one marker and a scrollable inspection panel. Declared
domain, function, and echelon are independent of the inventory. The panel
includes existing parent and subordinate relationships, current assignments,
ground stores, readiness, and equipment counts by type. Available includes
available and ready items. Unavailable includes the other surviving statuses,
including reserve and transit. The damaged count is a subset of surviving
equipment. Carrier and installation panels use authorized campaign records.

## 6. Seleção, inspeção e edição operacional

### Phase 1 implementation

The standalone viewer and CLI submit existing orders through the headless
`CommandSession`. It exposes a filtered snapshot, command permissions,
formation actions and cancellation eligibility. A command session reads
its own faction. Observer sessions may change their inspection perspective.

The viewer keeps one local draft with a mission type and destination.
Selecting a friendly formation offers actions it is equipped to perform.
On confirmation the campaign validates current authority, resources and
intelligence, then assigns a participant using the existing mission rules.
Choosing a geographic point submits a ground attack or ground defense.
Sector-based actions use the clicked strategic sector. Cancelling a
mission follows the same eligibility rule as the CLI, with a separate
confirmation in the viewer.

This phase implements draft review with destination and known control.
Route estimates and configurable waypoints remain part of the later
planning interface described below.

### 6.1 Seleção

Um clique seleciona unidade, contato, ponto geográfico, via ou instalação visível. O painel deve comunicar `tipo`, `fonte`, `última atualização`, `situação conhecida` e `ações possíveis`. Seleção múltipla pode agrupar ordens compatíveis; operações sobre entidades de facções distintas são rejeitadas. Em áreas de sobreposição visual, oferecer menu de candidatos filtrados e ordenados estavelmente.

### 6.2 Emissão de ordens

Ações disponíveis surgem de permissões e capacidades reais, não de listas estáticas irrestritas. Exemplos: atacar, defender, patrulhar, reconhecer, deslocar, ocupar, escoltar, solicitar suprimento, cancelar e alterar prioridade, **somente se suportadas pela engine e pela autoridade do operador**. Ações futuras não devem aparecer como executáveis até terem contrato implementado.

Fluxo mínimo:

1. Selecionar entidade própria autorizada ou objetivo.
2. Escolher ação compatível.
3. Criar rascunho local sem alterar campanha.
4. Definir participantes, alvo, posição/rota, janela e prioridades disponíveis.
5. Consultar validação e estimativa com carimbo de revisão/tick.
6. Confirmar; `CommandGateway` valida novamente no estado atual e aceita/rejeita.
7. Exibir resultado e razão detalhada; só uma ordem aceita gera compromisso e reserva.
8. Acompanhar execução, atraso, conclusão, falha ou cancelamento pelo snapshot posterior.

Erros típicos: sem autoridade; informações insuficientes; unidade indisponível; ausência de combustível/munição não reservado; rota impossível conhecida; alvo inválido; missão concorrente; capacidade de depósito insuficiente; informação obsoleta; estado alterado entre prévia e confirmação.

### 6.3 Correção de concorrência

O rascunho carrega `snapshot_revision`; confirmações obsoletas podem gerar recotação ou rejeição com detalhes. `request_id`/chave idempotente evita duplicação por duplo clique ou repetição do comando. O núcleo decide prioridades e reservas; mensagens devem diferenciar **prévia** de **ordem aceita**. Somente eventos confirmados integram o histórico reproduzível.

## 7. Planejamento de missões

### 7.1 Um único conceito de missão

Missões geradas pela IA e pelo jogador compartilham identificador, proprietário, tipo, objetivo, participantes, requisitos, estado e resultados. A atribuição `manual` ou a possibilidade de assumir uma aeronave é um atributo, não um modelo alternativo de missão. A UI do futuro jogo edita o mesmo plano que o standalone poderá executar abstratamente.

### 7.2 Fluxo de missão aérea

1. **Seleção:** escolher missão disponível, unidade aérea ou plataforma própria autorizada.
2. **Briefing:** objetivo, contexto, inteligência conhecida, restrições e regras de conclusão.
3. **Planejamento:** base de partida, alvo conhecido/estimado, waypoints geográficos ordenados, altitude pretendida quando suportada, horário e políticas de cancelamento.
4. **Recursos:** aeronave(s), armamento, combustível, manutenção, tripulação e disponibilidade conforme modelos realmente implementados.
5. **Validação:** alcance, capacidade de lançamento, concorrência de reservas, tempo, autoridade e risco conhecido; destacar incerteza inimiga.
6. **Confirmação:** emitir missão única no núcleo e receber ID/status.
7. **Execução:** standalone pela resolução abstrata; jogo 3D pelo executor tático, respeitando condições de transição.
8. **Debriefing:** perdas, consumo, observações, resultado e efeitos registrados na campanha.

`MissionDraft` pode conter campos planejados ainda não suportados por operações atuais, mas o frontend deve marcá-los como **não disponíveis** até existir validação real no núcleo; não fingir que definir waypoints altera missões abstratas caso a engine ainda os ignore.

### 7.3 Planejamento de rotas e pontos

- Waypoints são posições geográficas ou referências a objetos (ponte, cruzamento, setor/instalação, contato observado) resolvidas com metadados.
- Uma missão pode ter **alvo de setor** (objetivo estratégico) e **destino de posição** (ponto de reunião ou instalação). A UI distingue os dois.
- Estimativas de duração e combustível devem vir da mesma rotina de roteamento que o motor utiliza, com limites de inteligência e equipamento.
- Edição de rota informa segmentos indisponíveis, perigos conhecidos, pontos de passagem impossíveis e validade da própria informação.
- Traços desenhados com o mouse são *propostas*; a geometria final autorizada é confirmada pelo núcleo.

### 7.4 Conflitos entre IA e operador

A ordem humana autorizada prevalece apenas no escopo permitido e até cancelamento/expiração. A IA não deve duplicar objetivo, reaproveitar unidade já reservada ou desfazer ordem manual por mera otimização. Cancelar após execução não devolve recursos já gastos nem restaura posições anteriores. Em caso de conflito, a UI mostra qual ordem está bloqueando a nova operação.

## 8. Integração com o futuro jogo de helicópteros

A aplicação standalone é um produto válido por si só. O jogo tático poderá usar a interface antes de cada voo e após cada missão, sem absorver o motor estratégico para o loop do renderizador.

### 8.1 Contrato de entrega da missão

**Antes do voo:** o núcleo fornece identificador da campanha, versão/revisão, missão confirmada, aeronave/participantes, base/posição, horário, combustível, armamento, briefing, waypoints e inteligência autorizada. Esse conjunto é o *snapshot de inicialização tática*, sem concessão de autoridade sobre outros recursos da campanha.

**Durante o voo:** o simulador tático reporta eventos observáveis e ações físicas relevantes (lançamento, consumo, deslocamento, contato, dano, perdas, conclusão). Definir explicitamente quem avança o relógio da campanha e como conflitos com operações paralelas são resolvidos. **Nunca executar dois relógios autoritativos independentes** para o mesmo mundo.

**Depois do voo:** o motor valida e aplica resultados uma única vez, com IDs de evento/idempotência e ordenação de tempo. Missão, perdas, combustível, munição, inteligência e controle territorial continuam sob regras do núcleo. Resultado tático não captura setor arbitrariamente sem ocupação militar quando a regra de campanha assim exigir.

### 8.2 Políticas de simulação durante a missão

A implementação futura terá de escolher explicitamente entre: campanha avançando em tempo real simultaneamente ao voo, ou campanha suspensa/avançando por passos controlados. Esse é um **ponto de decisão futuro**, não algo decidido por este documento. Em qualquer opção: uma fonte de tempo, ordenação determinística dos eventos relevantes e reconciliação documentada.

### 8.3 Reutilização de arte e código

Compartilhar layout conceitual, mapeamento de símbolos, modelos de briefing, rascunhos, interfaces de consulta/ordem e testes. O jogo pode embutir a UI via biblioteca, ou integrá-la posteriormente pelo formato de snapshots/comandos; o mecanismo de integração será decidido quando existir o projeto tático. **Não** criar protocolo de rede antecipadamente.

## 9. Ciclo de atualização, threads e determinismo

- O núcleo avança em ticks; frontend recebe snapshots imutáveis de uma revisão conhecida. Separar atualização visual do processamento de ordens.
- O mapa pode renderizar a 30/60 FPS sem gerar ticks adicionais, reordenar eventos ou ler estruturas em mutação.
- Comandos recebidos entre ticks são enfileirados com IDs estáveis e efetivados na fase documentada; desempates não dependem da velocidade de mouse ou render.
- A visualização pode animar deslocamento entre posições conhecidas, mas não fabricar precisão inimiga nem corrigir as coordenadas reais.
- Ao pausar, rascunhos podem continuar sendo editados; ordens confirmadas só alteram estado na fase prevista.
- Eventos históricos e decisões tomadas são persistidos conforme o M8; câmera/zoom/painéis são preferências locais, não parte do determinismo militar.

## 10. Desempenho e legibilidade

O alvo inicial é o hardware de desenvolvimento modesto (8 GB de RAM, GPU integrada). Evitar desenho de todas as etiquetas e estradas em escala distante; usar culling geográfico, nível de detalhe cartográfico e organização visual por zoom. Renderizar símbolos vetoriais com cache, sem regenerar geometria em cada frame. Realizar picking somente sobre objetos visíveis e filtros autorizados.

- Mapas até 128×128 setores já foram validados na **simulação procedural abstrata**; isso não garante automaticamente performance do renderer geográfico.
- Testar pan/zoom com mapa grande e centenas de símbolos, frentes, rotas e painéis, medindo FPS, tempo de seleção e memória.
- Regras de agregação, layout e posicionamento de rótulos não podem afetar o estado da campanha.
- Informações textuais e ícones devem funcionar em modo de alto contraste, zoom da interface e telas de resolução limitada.

## 11. Integração com o M8 e artefatos

Persistir missões, ordens, alocações e respectivos eventos como parte da campanha. **Não** gravar somente a posição dos ícones ou o estado do editor como substituto de ordens efetivadas. Um rascunho não confirmado pode ser salvo separadamente nas preferências/projeto do usuário, mas não altera a guerra.

A UI deve reconhecer saves de versões compatíveis do motor e indicar rejeições por versões de regras não suportadas. É aceitável que bookmarks de câmera e painéis sejam guardados externamente ao save. IDs e referências de cenário devem permanecer estáveis para reabrir relatórios após carregamento.

## 12. Roadmap de implementação da interface

1. **UI-P0 — Contratos:** snapshots filtrados, contexto/autoridade, emissor de comandos, IDs estáveis e testes headless. Não construir outra engine de regras.
2. **UI-P1 — Mapa 2D:** mundo geográfico contínuo, pan, zoom, escala, seleção, camadas e tooltip; reutilizar o visualizador raylib quando conveniente.
3. **UI-P2 — APP-6:** catálogo versionado, renderer vetorial, afiliação relativa, função, escalão, rótulos e testes normativos, respeitando disponibilidade da edição autorizada.
4. **UI-P3 — Operador:** seleção de unidades, filtros, ordens autorizadas, feedback de validação/recusa e acompanhamento de missões.
5. **UI-P4 — Editor de missões:** alvo, participantes, waypoints, recursos, prévia e confirmação efetiva; o standalone já deverá ser utilizável como wargame de comando.
6. **UI-P5 — Relatórios e ergonomia:** histórico, inteligência, logística, tempo, save/load, atalhos configuráveis, legibilidade e desempenho.
7. **UI-P6 — Integração tática futura:** adaptador do jogo de helicópteros, selecionar/assumir missão, debriefing e devolução de resultados sem segunda campanha.

A câmera 3D livre com WASD e altura variável poderá ser adicionada como visualização complementar, **não faz parte das etapas obrigatórias deste documento**. Modelos 3D, terreno renderizado tridimensionalmente e física de voo pertencem à segunda fase do produto.

## 13. Matriz de testes e aceite

| ID | Cenário | Resultado obrigatório |
|---|---|---|
| UI-T01 | Operador observador tenta emitir ordem | Rejeição sem alteração do estado |
| UI-T02 | WEST e EAST consultam o mesmo contato | Cada um recebe apenas a própria visão/inteligência |
| UI-T03 | Alterar facção do operador | Afiliação gráfica é recalculada; não há vazamento |
| UI-T04 | Missão humana contra missão IA com unidade reservada | Uma única reserva efetiva; erro coerente para a concorrente |
| UI-T05 | Duplo clique em `Confirmar` | Uma missão/ordem efetivada, nunca duplicada |
| UI-T06 | Ordem baseada em snapshot obsoleto | Revalidar ou rejeitar, indicando divergência |
| UI-T07 | Criar missão via UI e via API direta | Mesmo estado e eventos efetivos |
| UI-T08 | Pausar, pan/zoom e seleção | Tick/RNG/resultado da campanha permanecem invariantes |
| UI-T09 | Selecionar contato inimigo estimado | Nenhuma posição ou estoque real oculto aparece |
| UI-T10 | Ordenar deslocamento para instalação dentro do setor | Alvo é ponto real, não apenas centro da célula |
| UI-T11 | Alterar zoom/agrupamento | Escalão e quantidade militares permanecem semanticamente corretos |
| UI-T12 | Renderização APP-6 | Composição, afiliação, escalão e modificadores validados contra edição autorizada |
| UI-T13 | Cores desativadas / alto contraste | Identidade e função continuam legíveis por forma e texto |
| UI-T14 | Save/load de campanha com missão ativa | Mesmas ordens, recursos, resultados e histórico após retomar |
| UI-T15 | Missão aérea standalone | Resolução abstrata sem dependência do jogo tático |
| UI-T16 | Missão player-flown (teste por adaptador simulado) | Mesmo ID de missão; resultado aplicado exatamente uma vez |
| UI-T17 | Relógio tático/campanha concorrentes | Nenhuma duplicação ou divergência de tempo autoritativo |
| UI-T18 | Mapa grande, muitas unidades | Desempenho mensurado sem modificar dados da simulação |

## 14. Definição de pronto

**Interface standalone completa:** operador seleciona facção/papel, observa mapa geográfico 2D, reconhece unidades por simbologia APP-6 implementada e validada no conjunto declarado, consulta informações autorizadas, cria e confirma missões com resultado de validação, emite ordens existentes, acompanha operações, controla tempo, salva e retoma campanha; o núcleo permanece executável sem UI.

**Preparada para o jogo de helicópteros:** o modelo de missão e o contrato de briefing permitem escolher uma missão pilotável e receber posteriormente os resultados por adaptador, sem replicar o editor, os dados militares ou as regras do motor. Implementar o simulador tático e os modelos 3D **não é requisito** para concluir o standalone.

**Simbologia:** o projeto poderá declarar conformidade **no escopo testado** somente quando códigos, funções, modificadores, escalões e regras gráficas forem confrontados com a APP-6 E/2 autorizada. Até lá, a documentação deve usar 'baseado em APP-6' e identificar desvios sem ambiguidade.

## 15. Decisões registradas e pontos ainda dependentes de implementação

**Decidido:** UI 2D reutilizável; mapa contínuo; setores como overlay; ordens validadas pelo núcleo; inteligência relativa à facção; APP-6 E/2 como referência; separação standalone/tático; edição de missão com mesma identidade; C++20 convencional e poucas dependências.

**A decidir apenas quando necessário:** biblioteca gráfica definitiva além do raylib existente; política de pausa durante voo tático; repertório inicial de símbolos cobertos por edição autorizada; layout final e atalhos padrão; se editor de missão aérea terá graus de liberdade avançados no primeiro lançamento; modo de incorporação no executável do jogo. Essas decisões não invalidam os contratos fundamentais.

## 16. Referências e rastreabilidade

- OpenWar: https://github.com/lucianobianc/openwar
- OTAN, APP-6, índice da publicação (Edição E, Versão 2, 16/10/2025): https://quicksearch.dla.mil/qsDocDetails.aspx?ident_number=275338
- OTAN, STANAG 2019, registro de acordo: https://quicksearch.dla.mil/qsDocDetails.aspx?ident_number=97629
- OTAN, explicação institucional de STANAG e padronização: https://www.nato.int/en/what-we-do/deterrence-and-defence/standardization
- Especificação geográfica complementar: `GEOGRAPHIC_WORLD.md`
- Anexo de ordens já existente: `OPERATIONS.md`
- Anexo de inteligência: `INTELLIGENCE.md`
- Anexo de cenários: `SCENARIOS.md`

**Política de manutenção:** esta especificação descreve o destino do produto e seus contratos, não afirma que todos os itens já estejam implementados. O `README.md` e o código vigente descrevem o estado executável. Alterações arquiteturais e divergências entre anexos devem ser explicitadas e resolvidas antes de codificadas.
