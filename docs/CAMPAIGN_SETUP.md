# Anexo 11 — Criação e configuração de campanhas

**Projeto:** Simulador de helicópteros com campanha dinâmica
**Versão:** 0.1 — 17 de novembro de 2026
**Estado:** especificação técnica proposta para implementação incremental.
**Documentos relacionados:** `DESIGN.md`, `TERRITORY.md`, `SCENARIOS.md`,
`MILITARY_FORCES.md`, `ECONOMY.md`, `LOGISTICS.md`, `MAINTENANCE.md`,
`INTELLIGENCE.md`, `OPERATIONS.md`, `COMBAT.md`, `TACTICAL.md`.

## 1. Objetivo

Ao escolher **Nova campanha**, o usuário deve poder começar rapidamente com
configurações recomendadas ou personalizar em detalhe mapa, forças WEST/EAST,
situação inicial, recursos, postura estratégica, objetivos e condições de
encerramento. O fluxo deve funcionar com cenários autorais, procedurais e
geográficos, inclusive mapas reais com forças fictícias.

Há **dois níveis de configuração, não duas engines de campanha**:

- **Simplificado:** poucas escolhas; valores restantes vêm de predefinições
  versionadas, transparentes e válidas.
- **Detalhado:** edição das mesmas configurações resolvidas, com controles
  por facção, composição de forças e situação de guerra.

Ambos produzem um `CampaignSetup` validado, resolvido por um **único** pipeline
em `WorldDefinition`, `ScenarioDefinition` e, após confirmação, `CampaignState`.
O modo escolhido só define a interface e a quantidade de escolhas expostas;
não muda as regras de simulação, dificuldade da IA ou nível de realismo.

### 1.1 Decisões consolidadas e propostas

**Consolidated:** WEST/EAST, separate geographic worlds and military definitions,
finite resources and autonomous campaigns. Real or explicitly authored
geography is the primary world input. The original procedural generator
continues to support legacy scenarios and regression tests.

The current workflow uses versioned text files and the CLI or viewer.
The setup wizard, previews, and broader presets below are design proposals.

Other established requirements include imperfect faction intelligence, player
orders subject to authority, conventional C++20, GNU Autotools, headless
execution, and support for modest Linux, FreeBSD, and Windows systems.

**Propostas deste anexo:** telas/ordem do assistente, modelos de configuração,
predefinições, presets de forças, parâmetros de vantagem, validações e formatos
adicionais. Nenhum exemplo numérico é dado histórico ou balanceamento final.

### 1.2 Invariantes

1. O assistente não modifica o estado de uma campanha em andamento.
2. Configuração não é cenário instalado, nem save: são três conceitos distintos.
3. A configuração final deve poder ser revisada, exportada e reproduzida.
4. Nenhuma vantagem escondida é concedida por escolher WEST ou EAST.
5. O mesmo setup resolvido, versão dos dados e seed produz o mesmo estado
   inicial, independentemente do caminho simplificado/detalhado.
6. Regras e parâmetros explícitos prevalecem sobre presets; jamais sobrescrever
   mudanças do jogador silenciosamente.
7. A tela de preparação pode mostrar **todos os dados iniciais de autoria**;
   iniciado o jogo, o mapa e a IA passam a usar somente `FactionView`, salvo
   depuração/observador autorizado.
8. É proibido alterar o mundo verdadeiro com informações da UI após o início.
9. Erros bloqueantes impedem iniciar; avisos informam consequências sem
   modificar o setup por conta própria.
10. Nenhum componente da UI é necessário para gerar ou validar uma campanha.

## 2. Fluxo geral de interface

```text
Menu principal
  ├─ Continuar / Carregar campanha (SaveGame; fluxo separado)
  └─ Nova campanha
        1. Escolher origem: cenário existente / gerar / importar mapa
        2. Escolher modo: Simplificado / Detalhado
        3. Configurar mundo e situação inicial
        4. Configurar WEST, EAST e participação do jogador
        5. Revisão: mapa, forças, recursos, objetivos e avisos
        6. Validar -> Confirmar -> Criar campanha -> Iniciar
```

A UI pode apresentar passos em ordem diferente, desde que preservados os
mesmos dados e validações. O usuário poderá voltar sem perder as alterações,
comparar com defaults, restaurar uma seção e abandonar o processo sem alterar
arquivos originais. Não exigir internet, conta, editor externo ou conhecimento
de programação.

O modo detalhado pode ser ativado a qualquer momento sem perder alterações.
Retornar ao simplificado **não descarta** ajustes detalhados: mostrar indicador
`Personalizado`; exigir confirmação explícita antes de aplicar um preset que
sobrescreva campos. O botão **Iniciar campanha** fica inativo diante de erros;
avisos não bloqueiam por padrão.

### Phase 4 launcher

The raylib application opens a launcher with no arguments. For new campaigns
it lists reusable geographic worlds, then the authored military scenarios that
reference each world. Combined demos and procedural configurations have a
separate list. The launcher shows declared description and objective metadata
when available. Full parsing and campaign initialization are shared with
direct startup.

A continued campaign uses the resolved checkpoint and the existing command
session. It offers Observer and the command role granted by the checkpoint.
New campaigns can select a participating commander when the scenario format
supports command. Procedural configurations retain their observer-only startup
behavior.

The launcher can open local content through an in-app browser, a pasted path,
file drop, or an optional Unix system dialog. Save and Menu in the viewer
write a numbered checkpoint and return to the launcher, respectively.
The [viewer guide](../README.md#campaign-viewer) documents install and user
data paths.

## 3. Origem do mundo e cenário-base

The world source and military composition are separate decisions. The current
file-based workflow loads an `OWGEO 2` geographic world and resolves a military
scenario from `OWSCENARIO 1`. Creating a campaign produces mutable state, which
can later be saved to a checkpoint. The table includes future combinations.

| Origem | Mundo | Forças iniciais |
|---|---|---|
| Cenário instalado/autoral | Definido pelo pacote | Prescritas ou editáveis por cópia |
| Mapa procedural | Gerado por seed | Definidas ou geradas por orçamento |
| Mapa geográfico | Importado de dados reais | Históricas, fictícias ou geradas |
| Híbrido | Qualquer das anteriores | Mistura de valores explícitos e gerados |

Cada pacote declara quais campos podem ser alterados e quais são fixos por
restrição técnica. O sistema sempre permite **duplicar como variante**, quando
as licenças dos dados permitirem. Alterar um cenário instalado não o sobrescreve.
Em cenários históricos, a opção `historical` é apenas um conjunto de dados
fornecido pelo autor; nunca inventar ordem de batalha histórica automaticamente.

### 3.1 Seleção de mapa real

Exibir região, extensão, resolução/precisão conhecida, origem e licença dos
dados, cobertura de elevação/estradas e advertências sobre dados ausentes.
Mapas reais podem usar implantação e objetivos fictícios. Importação GIS ocorre
em ferramenta/pipeline do `SCENARIOS.md`; não implementar acesso online na UI
inicial. Coordenadas, projeção e escala são validadas antes de criar setores.
A ausência de dados de instalações militares **não implica** bases reais.

### 3.2 Geração procedural

Future procedural military generation should place forces, stocks, and
objectives on a selected geographic world, using an explicit seed. The
paragraph below describes a possible UI for the legacy terrain generator.

Oferecer campo de seed, opção de gerar outra seed e prévia do mundo. Parâmetros
mínimos: tamanho, relevo predominante, água, densidade viária e infraestrutura.
As faixas devem refletir os limites do gerador versionado. Prévia e execução
usam a **mesma definição resolvida**: proibir prévia que regenere outra malha
sem aviso. Não usar RNG de renderização ou relógio de parede na resolução.

## 4. Modo simplificado

Objetivo: iniciar campanha coerente sem editar inventários ou parâmetros
numéricos. Fluxo sugerido:

**Tela 1 — Mapa:** escolher um cenário existente ou gerar um mapa; mostrar
miniatura, dimensão, tipo de terreno e resumo de conteúdo.

**Tela 2 — Situação:** selecionar uma das situações: `balanced_front`,
`west_offensive`, `east_offensive`, `west_defensive`, `east_defensive`,
`meeting_engagement`, `custom` (somente se importada). Cada uma aplica um
**template documentado** de implantação, postura e objetivos; não modifica a
capacidade intrínseca de veículos. Ações de invasão/início de ofensiva exigem
rotas e setores iniciais adequados.

**Tela 3 — Intensidade da guerra:** `small`, `medium`, `large`. Define orçamento
inicial de forças, densidade e capacidade de sustento **de ambas as facções**;
não é multiplicador oculto de HP ou dano. Limites respeitam tamanho do mapa.

**Tela 4 — Equilíbrio:** `balanced`, `west_advantage`, `east_advantage` ou
`asymmetric`. Ajusta explicitamente os orçamentos relativos e a capacidade
inicial de sustentação. Equilíbrio significa aproximadamente equivalente em
**valor de capacidade**, não o mesmo número de veículos.

**Tela 5 — Jogador:** WEST/EAST, papel `pilot`, `squadron`, `operational` ou
`observer`, e base inicial se o papel permitir. A facção escolhida não recebe
recursos extras por padrão.

**Tela 6 — Resumo:** mapa, linha inicial, número e tipos principais de unidades,
estoques agregados, objetivos, reforços, previsão de capacidade de manutenção,
seed, condições de vitória, advertências e botão Iniciar.

Todos os defaults devem estar no arquivo de preset versionado, não espalhados
em callbacks da UI. A interface mostra o que o preset modificou. Uma campanha
simplificada usa integralmente manutenção, moral, combustível, logística e
reconhecimento da simulação — sem atalhos de mecânica.

## 5. Modo detalhado

A interface detalhada começa com as escolhas simplificadas **já resolvidas**.
Permite editar seções de forma independente e exibe diferença em relação ao
preset de origem.

### 5.1 Mundo e divisão territorial

- Origem do mapa, seed, dimensões, terreno e malha de setores.
- Controle inicial por setor, neutralidade, disputa e guarnições válidas.
- Centros de comando, cidades, bases, depósitos, instalações industriais,
  estradas e rotas, quando presentes no mundo.
- Alteração de dados do mundo deve respeitar validação topológica; mudar
  tamanho/gerador pode invalidar posições. A UI mostra esses conflitos e
  exige resolver ou regenerar **explicitamente**.
- Conquista posterior continua governada por `TERRITORY.md`; configurar o
  mapa inicial não dá controle via simples presença aérea.

### 5.2 Forças WEST e EAST

Exibir painel equivalente para cada lado. O jogador poderá:

- Escolher catálogo de era/pacote e tipos disponíveis, respeitando
  `MILITARY_FORCES.md` e disponibilidade dos assets.
- Definir quantidade por tipo (ex.: AH-64A, Mi-24V/P, M1A1, T-72B, SAM,
  MANPADS, transporte, artilharia e infantaria abstrata).
- Distribuir unidades em formações, bases, setores e reservas.
- Ajustar o efetivo inicial, experiência, moral, coesão, fadiga, prontidão,
  condição do equipamento e ocupação das instalações conforme os limites
  especificados pelos sistemas responsáveis.
- Definir postura inicial e missões iniciais: defesa, patrulha,
  reconhecimento, ofensiva ou reserva.
- Configurar reforços externos: tipos, quantidades, disponibilidade,
  condições de chegada e janelas; sem gerar recursos gratuitamente.

**Três meios de configuração:** (a) escolher uma ordem de batalha predefinida;
(b) definir um orçamento por facção e gerar composição plausível;
(c) editar unidades e quantidades manualmente. O modo (c) pode ultrapassar
orçamentos se o cenário permitir, **com aviso de desequilíbrio**, não com bônus
implícitos. A interface deve distinguir tipos autorizados, contagem de
exemplares, unidades de formação e exemplares operacionais.

### 5.3 Potência militar e assimetria

Não usar um único multiplicador global de força. Para compor presets, definir
`force_budget` como **medida interna de geração**, calculada a partir de custos
relativos versionados por tipo/pacote. Seus valores não representam preço real.
Custos consideram capacidade de combate e demanda logística estimadas.
Ajustes separados por facção: força terrestre, helicópteros, defesa aérea,
artilharia, apoio logístico, estoques, reservas e capacidade de manutenção.

A geração deve respeitar: compatibilidade com bases, tripulação, estoque de
armamento, capacidade de manutenção, rotas, terreno e infraestrutura.
Se for inviável, produzir diagnóstico e não inserir unidades extras por fora
do orçamento. Quantidades geradas devem ser exibidas antes da confirmação.

### 5.4 Situação inicial e escalada

Configurar: setores controlados, pontos de contato, posição das formações,
posturas dos comandantes, operações planejadas, intensidade inicial de combate,
nível de mobilização, estado das infraestruturas e conflitos em curso.

Perfis propostos:

| Perfil | Condição inicial | Expectativa |
|---|---|---|
| `balanced_front` | Fronteira estabelecida e forças em alerta | Guerra em equilíbrio relativo |
| `west_offensive` | WEST concentrada em eixos de avanço | EAST reage/defende |
| `east_offensive` | EAST concentrada em eixos de avanço | WEST reage/defende |
| `meeting_engagement` | Forças manobram para objetivos disputados | Contatos em formação |
| `west_defensive` | WEST guarnecida, EAST pressionando | Defesa assimétrica |
| `east_defensive` | EAST guarnecida, WEST pressionando | Defesa assimétrica |

Perfis definem **condições iniciais**, não roteiros obrigatórios. Após iniciar,
a IA pode mudar de plano e ambos os lados podem vencer. Não aplicar efeitos
não declarados de moral ou vantagens apenas por nome de perfil.

### 5.5 Economia, logística e manutenção

Editar separadamente para WEST/EAST: estoques e capacidade de cada depósito,
insumos e taxas de produção, estado de usinas/refinarias/oficinas, frota de
transporte, rotas habilitadas, equipes de reparo, capacidade de hangares,
peças, tempos de preparação e condições iniciais dos equipamentos.

Validar balanço: recursos armazenados em instalações, origem dos reforços e
compatibilidade de infraestrutura. Os parâmetros de consumo e reparo seguem
os anexos responsáveis; o assistente não implementa sua própria economia.

### 5.6 Informações iniciais de inteligência

Configurar reconhecimento inicial por facção: área observada, contatos
confirmados, contatos prováveis, última observação e confiança. **Nunca
inicializar o conhecimento de uma facção com toda a implantação inimiga só
porque o autor do cenário a definiu.** Uma escolha opcional `full_reveal`
seria uma regra explícita de cenário especial, nunca default; requer indicar
se é simétrica ou não. A UI pré-início pode mostrar a visão de cada facção
separadamente, para revisão da informação disponível.

### 5.7 Vitória, duração e participação

Configurar as condições de `TERRITORY.md`: capitulação, objetivos críticos,
janela de confirmação, reforços ainda disponíveis, tempo limite opcional,
impasse e modo sandbox. Não definir vitória por mero percentual de setores.
Exibir as regras em linguagem clara, inclusive as condições habilitadas.

Escolher lado e autoridade do jogador (`OPERATIONS.md`), eventuais restrições
de escolha de base e possibilidade de intervenção sobre a IA. A autorização
para editar forças ocorre **antes** da campanha; autoridade operacional durante
a campanha é uma regra separada.

## 6. Modelo de dados e resolução

Contratos conceituais (nomes indicativos, não obrigação de classes):

```text
CampaignSetupDraft:
  schema_version
  scenario_reference / world_generation_parameters
  world_overrides
  situation_preset_id
  force_presets_and_overrides[WEST, EAST]
  economy_logistics_maintenance_overrides
  initial_intelligence_overrides
  player_faction_and_authority
  victory_overrides
  seed

ResolvedCampaignSetup:
  world_definition
  scenario_definition
  resolution_metadata (preset versions, generator version, seed)
  warnings
  reproducibility_digest
```

Pipeline **headless**:

```text
load_preset_or_scenario
 -> apply_explicit_user_overrides
 -> resolve_world (import/generate/read)
 -> resolve_deployment_and_installations
 -> resolve_resources_orders_and_initial_intel
 -> validate_all_invariants
 -> show_preview_and_diagnostics
 -> user_confirms
 -> create_campaign_state
 -> save_initial_snapshot
 -> start_simulation
```

Se o usuário mudar seed, mapa, perfil ou catálogo de forças, marcar como
`stale` apenas os produtos derivados afetados. Regenerar esses produtos após
pedido explícito; preservar valores fixados manualmente que continuem válidos;
mostrar conflitos de referências em vez de relocação silenciosa.

**Precedência:** regras do engine > restrições de schema > dados do cenário-base
> preset selecionado > overrides explícitos do jogador, exceto quando o cenário
declara campo travado; travas são visíveis. Esta ordem significa que um
*override* explícito vence defaults, mas nunca viola invariantes. Não
reinterpretar campos silenciosamente.

## 7. Presets, variantes e arquivos do usuário

- `Preset`: receita versionada para resolver defaults; não é SaveGame.
- `ScenarioPackage`: dados de mundo e/ou forças, conforme `SCENARIOS.md`.
- `CampaignSetup`: seleção + mudanças do usuário, preservável para reutilizar.
- `SaveGame`: estado da campanha **após** inicialização, independente do
  pacote de origem conforme regras de persistência de `SCENARIOS.md`.

Permitir **Salvar configuração como...**, **Duplicar cenário**, **Exportar
cenário inicial** e **Compartilhar pacote**. Não sobrescrever pacote instalado,
a menos que o usuário tenha explicitamente permissão e escolha fazê-lo.

Formato de setup proposto: TOML, compatível com manifestos de `SCENARIOS.md`.
Para não criar dois esquemas concorrentes, registrar overrides em seção
`[campaign_setup]` ou arquivo auxiliar com referência ao cenário original.
O schema completo será congelado junto ao parser e testado com round-trip.

Exemplo ilustrativo mínimo:

```toml
schema_version = 1
scenario_id = "rolling-hills"

[campaign_setup]
mode = "detailed"   # preferência de UI, sem efeito na simulação
situation = "east_offensive"
seed = 12345
player_faction = "WEST"
player_authority = "squadron"

[campaign_setup.force_generation]
west_budget = 100
east_budget = 130

[campaign_setup.victory]
max_campaign_days = 30
```

Este exemplo **não é esquema pronto para parser**: `SCENARIOS.md` permanece
autoritativo quanto a `schema_version`, `world.source`, `deployment.source`,
IDs e estrutura do pacote. O loader definitivo deve definir como um
`CampaignSetup` referencia um manifesto válido sem duplicar campos.

## 8. Revisão e feedback antes de iniciar

A tela de resumo deve mostrar:

1. Nome, origem e versão do cenário; mapa/previsão de memória e seed.
2. Miniatura do terreno, controle territorial inicial e instalações-chave.
3. Quadro comparativo WEST/EAST: forças operacionais, reservas, logística,
   manutenção, estoques e reforços previstos.
4. Condições iniciais: posturas, conflitos já em curso e objetivos.
5. Visões de inteligência iniciais por facção, separadas da verdade do autor.
6. Facção do jogador, autoridade e base inicial.
7. Condições de encerramento e duração máxima opcional.
8. Diagnósticos `ERROR`/`WARNING`, com caminho para corrigir o campo.

Não apresentar "equilibrado" sem uma métrica do preset; se a estimativa de
poder militar for imprecisa, rotular como **estimativa**, nunca como garantia
de chances iguais. Prévia não executa resultados futuros da guerra como se
fossem certos.

## 9. Validação e mensagens

Validação reutiliza os invariantes de `SCENARIOS.md` e dos anexos de domínio.
Checagens adicionais do assistente:

- Preset e versão existentes; locks explícitos e overrides permitidos.
- WORLD: escala, setores, travessias, instalações e origem dos dados válidos.
- FORCES: tipos conhecidos, IDs estáveis, posição alcançável, base compatível,
  tripulações e reservas sem duplicações.
- LOGISTICS: estoques não negativos, meios, caminhos e capacidade inicial;
  advertir sobre isolamento ou incapacidade de sustentar a força escolhida.
- SITUATION: frentes e objetivos coerentes, eixos ofensivos acessíveis,
  setores críticos atribuídos de maneira válida.
- PLAYER: facção existente, autoridade válida, base elegível se necessária.
- INTEL: contatos declarados com fontes/tempos coerentes; sem vazamento.
- VICTORY: condições e tempo válidos, sem campanha que já terminou no tick 0
  por acidente; permitir condição inicial terminal somente em teste explícito.
- PERFORMANCE: orçamento estimado de entidades, setores e ativos 3D dentro
  dos limites configurados; advertir antes de alocações caras.

`ERROR` bloqueia o início e indica seção/campo/ID; `WARNING` permite
continuar mediante revisão, sem correção automática. A validação deve poder
ser invocada sem UI, usando a mesma função do assistente. Um arquivo de
usuário defeituoso não deve inutilizar os demais cenários.

## 10. Integração, determinismo e persistência

`CampaignSetup` resolve definições estáticas; não possui ponteiro para
renderizador, não conhece FPS e não roda IA operacional. A engine usa estado
resolvido único. Nenhum setor, unidade, estoque ou observação é criado depois
do início apenas porque o assistente ficou aberto.

Armazenar metadados de provenance (IDs/versões de presets, arquivo de origem,
seed, gerador, hash canônico). Salvar o mundo inicial resolvido e a campanha,
conforme `SCENARIOS.md`; carregar save **não executa novamente** o wizard ou
o gerador. Reabrir uma configuração salva recompõe escolhas, sem depender do
estado mutável de campanhas anteriores.

Determinismo exige desempates estáveis, RNG com estado explícito e
serialização canônica. Seleções manuais fixas não devem consumir RNG.
Resultados da geração não devem depender da ordem de renderização da UI.

## 11. API mínima (conceitual)

```text
list_available_scenarios() -> ScenarioSummary[]
load_campaign_setup(path_or_preset) -> CampaignSetupDraft
apply_setup_changes(draft, changes) -> CampaignSetupDraft
resolve_campaign_setup(draft) -> ResolvedCampaignSetup | Diagnostics
validate_campaign_setup(resolved) -> Diagnostics
preview_campaign_setup(resolved, perspective) -> PreviewData
export_campaign_setup(draft, destination) -> Result
create_campaign(resolved) -> CampaignState | Diagnostics
```

`preview_campaign_setup` admite visão de autor antes do início e visões
WEST/EAST separadas. Após criar a campanha, a UI normal consulta apenas
`FactionView`. Evitar expor `WorldState` como DTO comum aos frontends.
A persistência de drafts é separada de saves.

## 12. Ordem de implementação

| Etapa | Entrega verificável | Limites |
|---|---|---|
| C0 | Modelo headless de setup + presets estáticos e validação básica | Sem UI, sem gerador GIS |
| C1 | Loader/serializer, alterações, round-trip e exportação de variante | Sem gameplay |
| C2 | Gerador procedural de forças para mapas já suportados | Sem novas regras de combate |
| C3 | Prévia e fluxo simplificado no frontend 2D | Usa mesmos serviços headless |
| C4 | Editor detalhado de forças, situações, recursos e inteligência | Não modifica campanha em curso |
| C5 | Integração com mapas geográficos, importação e variantes compartilháveis | Sem dependência de rede em runtime |

**Não antecipar etapas por conta própria**; construir após os módulos de
domínio dos quais cada opção depende. O M0 atual apenas separa definição de
mundo, definição de cenário e estado mutável, sem implementar esse wizard.

## 13. Testes automatizados de aceitação

| ID | Caso | Resultado obrigatório |
|---|---|---|
| CS01 | Mesmo preset/seed, simplificado e detalhado sem overrides | Mesmo digest de estado inicial |
| CS02 | WEST/EAST selecionados alternadamente pelo jogador | Mesmo mundo e forças iniciais |
| CS03 | Mudar para detalhado e voltar | Mudanças preservadas; sem perda silenciosa |
| CS04 | Aplicar preset sobre overrides manuais | Aviso e confirmação antes de sobrescrever |
| CS05 | Duplicar cenário instalado e editar variante | Original byte a byte inalterado |
| CS06 | Cenário geográfico + forças fictícias | Inicializa corretamente |
| CS07 | Mapa procedural + forças definidas manualmente | Mantém inventário explícito |
| CS08 | Mesmo seed e versão de gerador | Mesmo mapa, ordens e inventários |
| CS09 | Alterar seed após edição manual | Não substitui alterações fixadas; avisa conflitos |
| CS10 | Orçamento assimétrico | Quantidades explícitas, sem bônus de dano oculto |
| CS11 | Equipamento sem base/capacidade apropriada | Erro ou aviso documentado; sem correção silenciosa |
| CS12 | Ofensiva sem rota de avanço válida | Diagnóstico aponta eixo/objetivo inválido |
| CS13 | Faltar logística para sustentar forças | Aviso de viabilidade, sem criação mágica de suprimentos |
| CS14 | Cenário inicial com contato inimigo desconhecido | FactionView não mostra localização verdadeira |
| CS15 | Visão de autor pré-início e visão WEST/EAST | Não há vazamento após início |
| CS16 | Vitória habilitada no tick 0 acidentalmente | Erro antes de iniciar |
| CS17 | Salvar e recarregar setup | Mesmo conteúdo resolvido e mesmas escolhas |
| CS18 | Carregar save existente | Não reroda geradores nem wizard |
| CS19 | Pacote de terceiro com erros | Não bloqueia outros cenários; diagnóstico legível |
| CS20 | Seed/versão/config iguais após exportar variante | Mesma campanha inicial |
| CS21 | Autoria com recursos negativos/IDs duplicados | Erro bloqueante com localização |
| CS22 | Confirmação de setup válido | Um único estado inicial e snapshot persistente |
| CS23 | Usuário escolhe `observer` | Não recebe autoridade de emitir ordens |
| CS24 | Preview de mapa grande fora do limite | Diagnóstico antes de reservar memória excessiva |

## 14. Critérios de pronto

O recurso está pronto quando um usuário consegue: (a) selecionar cenário
instalado ou gerar mundo; (b) escolher modo simplificado ou detalhado sem
perder parâmetros; (c) ajustar forças, assimetria, estoques e situação inicial;
(d) revisar exatamente o que começará, incluindo warnings; (e) iniciar
campanha coerente e reproduzível; (f) salvar variante própria sem modificar
cenários instalados; (g) realizar tudo isso sem alterar o núcleo headless da
campanha; (h) carregar um save sem regenerar o mundo.

**Decisões abertas a resolver quando o módulo for implementado:** formato final
do setup (anexo do manifesto ou arquivo dedicado), métricas/calibração do
orçamento de forças, limites de mapa por perfil de hardware, conjunto final
de templates e linguagem/localização da interface. Nenhuma dessas decisões
bloqueia o M0.
