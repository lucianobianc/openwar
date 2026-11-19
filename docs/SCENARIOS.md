# Anexo 10 — Cenários, mapas reais e geração procedural

**Projeto:** Simulador de helicópteros com campanha dinâmica
**Versão:** 0.1 — 17 de novembro de 2026
**Estado:** especificação técnica proposta; não autoriza implementação antecipada.
**Documentos relacionados:** `DESIGN.md`, `TERRITORY.md`, `ECONOMY.md`,
`LOGISTICS.md`, `MILITARY_FORCES.md`, `INTELLIGENCE.md`, `OPERATIONS.md` e
`ANNEXES_README.md`.

## 1. Objetivo e decisões

O usuário deve conseguir: (a) iniciar uma campanha definida manualmente;
(b) gerar uma campanha fictícia por parâmetros e semente; (c) usar geografia de
uma região real; (d) criar, editar, compartilhar e instalar cenários **sem
programar nem recompilar C++**; (e) salvar e retomar qualquer campanha.

**Decisões consolidadas:** duas facções iniciais `WEST` e `EAST`, guerra
autônoma, setores com controle territorial, inteligência imperfeita e um único
estado autoritativo; simulação sem interface gráfica; C++20 legível, GNU
Autotools e poucas dependências; hardware modesto (i5-1135G7, Iris Xe, 8 GB).

**Current implementation.** `OWGEO 2` holds a reusable physical world.
`OWSCENARIO 1` references it and declares the initial military situation.
`CampaignState` holds the live simulation and saves its resolved state to
a checkpoint. The original `OWGEO 1` combined demo and its procedural
contemporaries remain supported.

Directory packages, general TOML manifests, and optional CSV tables in
sections 3–5 are earlier design proposals.
TOML é uma **proposta de formato**, não uma obrigação de escrever um parser do
zero. Confirmar o formato concreto antes de implementar serialização. Os
exemplos abaixo são normativos como *modelo de dados*, mas não exigem que o M0
já consiga interpretá-los.

### 1.1 Princípios invariantes

1. Cenário predefinido e procedural entram no **mesmo inicializador** e produzem
   a mesma representação de `WorldState`.
2. Conteúdo de cenário é dado, não código executável; arquivos de usuário não
   podem executar scripts nem alterar regras internas da engine.
3. **Seed não torna um cenário definido aleatório:** apenas escolhas declaradas
   como probabilísticas usam RNG. Configurações explícitas permanecem fixas.
4. Nenhum veículo, recurso, terreno, conhecimento ou reforço aparece sem origem
   definida pelo cenário, pelas regras da campanha ou por um evento registrado.
5. Um mapa geograficamente real pode abrigar uma guerra fictícia. Usar topografia
   real **não implica** dados militares reais ou reconstituição histórica.
6. Dados espaciais estáticos são compartilháveis; mudanças de controle,
   destruição de infraestrutura, estoques e inteligência pertencem à campanha.
7. Cenários são independentes de renderização 3D; geometria de setores e
   topologia permanecem úteis com a interface desligada.
8. Erros de referências e de formato produzem diagnósticos claros, não falhas
   silenciosas ou defaults que modifiquem a situação militar sem aviso.

## 2. Três camadas de dados, três operações distintas

| Camada | Contém | Pode mudar durante campanha? |
|---|---|---|
| Geographic world | Spatial frame, relief, water, strategic grid, crossings, roads and geographic features. In C++, `geography::Map` plus the derived `WorldDefinition` grid. | Reusable physical source |
| `ScenarioDefinition` | seleção de mundo, facções, inventários, implantação, estoques, instalações operantes, comando, objetivos, reforços e regras de término | Não: é a condição inicial |
| `CampaignState` | relógio, RNG, ocupação, unidades, recursos, danos, ordens, observações, eventos e guerra em andamento | Sim, exclusivamente pelas regras da simulação |

`resolve_world(input)` gera ou importa um `WorldDefinition` **validado**;
`resolve_scenario(input, world)` gera ou lê um `ScenarioDefinition`
**validado**; `start_campaign(world, scenario, seed)` cria `CampaignState`.
São etapas conceituais, não exigência de interfaces C++ particulares.

`SaveGame` é diferente de `ScenarioDefinition`. Nunca usar o arquivo original de
cenário como save, nem reexecutar a geração procedural ao carregar um save.

### 2.1 Modos suportados

- **`authored`:** mundo e forças descritos explicitamente por arquivos; para
  teatros de ficção, reconstituições e cenários de terceiros.
- **Legacy procedural:** terrain, infrastructure and initial military
  state generated from parameters and a seed. Available for demos and regression.
  Future military generation can consume the same geographic input as authored
  deployments.
- **`geographic`:** terreno/base cartográfica derivados de arquivos geográficos
  reais, com forças explicitamente definidas **ou** geradas opcionalmente;
  dados reais não forçam um inventário histórico.
- **Híbrido:** usar um mundo importado ou autoral e apenas gerar implantação,
  composição de forças e estoques (ou vice-versa). Modelar como escolhas
  independentes `world.source` e `deployment.source`, não como uma quarta engine.

### 2.2 Current format OWSCENARIO 1

Use a UTF-8 `.owscenario` file, with one record per line. Blank lines and
comments starting with `#` are allowed. The first two records are
`OWSCENARIO 1` and `world "relative/path.owgeo"`. The path is resolved from the
scenario directory and must identify an `OWGEO 2` world. Strings shown in
quotes are required to be quoted. Define names and numeric IDs before
referencing them.

| Record and fields in order | Meaning |
|---|---|
| `# @name "TITLE"` | Optional launcher title, up to 120 UTF-8 bytes. |
| `# @description "TEXT"` | Optional launcher description, up to 1024 UTF-8 bytes. |
| `point NAME E S UP` | Named geographic location. All coordinates are local millimetres. East and south start at the northwest origin. |
| `control SECTOR SIDE POINTS` | Initial controller and objective points. Unlisted sectors start neutral. |
| `public SECTOR` | Add the initial territorial report to both factions. Each faction already receives reports about its own sectors and installations. |
| `objective SIDE POINT PRIORITY` | Operational objective with a positive priority and a geographic destination. Objectives in one sector share its destination. |
| `victory SIDE HOLD_SECONDS SECTOR` | A sector-hold victory condition. |
| `command_site SIDE SECTOR` | A sector relevant to command collapse under the existing rules. |
| `stock ID SIDE POINT CAPACITY QUANTITY` | Fixed stockpile, or use `-` for the dedicated mobile stock of a ground unit. |
| `installation ID STOCK WORKERS POWERED INTEGRITY INTERVAL REQUIRED NEEDS_POWER INPUTS OUTPUTS "NAME"` | Industrial installation and production recipe. Stock provides its location and faction. Integrity is 0–100. The batch interval uses economy intervals. |
| `facility ID STOCK BAYS TECHNICIANS OPERATIONAL` | Maintenance facility at a fixed stockpile. |
| `type ID SIDE CATEGORY POWER OCCUPIES SPEED_KPH RANGE SERVICE_SECONDS REPAIR_SECONDS REPAIR_PARTS MOBILITY "NAME"` | Equipment type. Range is in sectors. Service seconds set the service interval. The three other service profiles use engine defaults. |
| `unit ID SIDE POINT MOBILE_STOCK FACILITY MORALE "NAME"` | Ground units use a dedicated mobile stockpile and may use a local facility. A unit without mobile stock must deploy at its facility. Use 0 for an omitted reference. |
| `identity UNIT DOMAIN FUNCTION ECHELON` | Optional declared military identity. Use after the unit record. |
| `parent CHILD PARENT` | Optional existing command relationship. Declare both same-faction units first. |
| `equipment ID TYPE UNIT` | Assign one starting equipment item. |
| `replacement ID TYPE UNIT "SOURCE"` | Assign one finite reserve item to a formation with a facility. The source is a label. The item starts in reserve and uses normal reinforcement entry and transit rules. |
| `carrier ID SIDE POINT CAPACITY FUEL MOBILITY` | Logistics carrier and initial fuel in litres. |
| `shipment ID ORIGIN DESTINATION RESOURCE QUANTITY PRIORITY` | Initial supply request. |
| `resupply ORIGIN DESTINATION RESOURCE THRESHOLD TARGET` | Recurring supply policy. |
| `timing TICK_SECONDS ECONOMY_TICKS LOAD_TICKS UNLOAD_TICKS TIME_LIMIT_SECONDS` | Optional clock and timing record. Zero time limit disables the limit. |
| `operations PLANNER_TICKS STRATEGY_TICKS MAX_ORDERS TERRITORY_HALF_LIFE CONTACT_HALF_LIFE INSTALLATION_HALF_LIFE` | Optional planning settings. Use 0 to disable an interval. Half-lives use seconds. |
| `combat INTERVAL_TICKS OCCUPATION_SECONDS CRITICAL_SECONDS STABILIZATION_SECONDS CAPITULATION_SECONDS REINFORCEMENT_HORIZON_SECONDS` | Optional abstract combat settings. |
| `force_recovery ENABLED` | Optional strategic-planner workshop returns. Use 1 to enable or 0 for the default. |

The optional display comments can appear once each after the world reference.
Existing OWSCENARIO 1 loaders treat them as comments. The launcher uses the
file name when a title is absent. These fields do not enter the campaign
fingerprint or checkpoint.

Identity fields accept the following case-sensitive values.

- Domain is `land`, `air`, `sea` (surface), or `unspecified`.
- Function is `infantry`, `armor`, `mechanized-infantry`, `reconnaissance`,
  `artillery`, `engineer`, `signals`, `transport`, `support`, or `unspecified`.
- Echelon is `platoon`, `company`, `battalion`, `regiment`, `brigade`,
  `division`, or `unspecified`.

For example, `identity 1 land mechanized-infantry battalion` declares the
organization of unit 1 independently of any `equipment` records. Omitted
identity defaults to unspecified. Cyclic command relationships are invalid.

Checkpoint schemas from 6 onward store the declared identity for each military
unit. The reader accepts the previously supported schema and rules versions
and supplies unspecified identity for older checkpoints. Saving a migrated
checkpoint writes the current schema 11 with rules version 9. The scenario
fingerprint includes declared identity.

Faction names are `WEST` and `EAST`. `NEUTRAL` can own sectors and fixed
stocks. Flags use 0 or 1. Resource vectors list seven nonnegative integers
in the order `oil_reserve crude_oil fuel materials spare_parts supplies ammunition`.
Shipment and resupply records name a single resource. The categories are
`helicopter`, `tank`, `transport`, `air_defense_vehicle`, and `air_defense_team`.
Mobility is `default` (the engine profile) or `valley` (the original
Connecticut Valley demonstration profile). Omitted settings use the
campaign defaults with combat and operations enabled and automatic planning
disabled. Select a permitted CLI faction at launch.

The loader derives sector membership from points and validates references.
The regular campaign initializer validates operational, economic, and
ownership constraints. See the [original deployment](../examples/connecticut-valley/valley-demo.owscenario)
and the [autonomous variant](../examples/connecticut-valley/valley-autonomous.owscenario)
for complete examples. Both reference the same `valley-world.owgeo`.

## 3. Proposed installable scenario packages

Estrutura proposta — *não* criar todos os arquivos se desnecessários:

```text
scenarios/
└── border-valley/
    ├── scenario.toml          # obrigatório: metadados e origem dos dados
    ├── world/                 # se autoria/importação explicitamente definida
    │   ├── sectors.csv        # atributos por setor, opcionais
    │   ├── roads.csv          # grafo de transporte, opcional
    │   └── elevation.png      # relevo para renderização, opcional
    ├── units.csv              # unidades e implantação, opcional
    ├── installations.csv      # instalações, opcional
    └── README.md              # descrição livre, opcional
```

Regras de descoberta:

- Ler diretórios de cenários no caminho indicado por `--scenario <path>` e
  opcionalmente em diretórios de usuário/sistema documentados. A escolha da
  convenção XDG e dos caminhos exatos fica para o frontend.
- **Não** exigir cadastro online, ferramenta gráfica, recompilação ou
  alterações no repositório para adicionar um cenário.
- `id` único por pacote, caracteres ASCII minúsculos, dígitos e hífens; nomes
  exibidos aceitam Unicode. Identificadores de facções iniciais continuam
  `WEST` e `EAST`.
- Caminhos relativos são resolvidos dentro do pacote. Rejeitar `../` que escape
  da raiz, paths absolutos não autorizados e links simbólicos que escapem.
- Erro em pacote de terceiros não impede abrir outros cenários.
- Uma pasta com `scenario.toml` válida é suficiente para um cenário procedural;
  um cenário autoral pode adicionar CSVs aos poucos.
- Arquivos adicionais desconhecidos são permitidos; chaves de schema
  desconhecidas relevantes são relatadas (erro em modo estrito).

## 4. Arquivo `scenario.toml`: contrato de alto nível

TOML foi escolhido como *proposta* porque permite edição manual e comentários.
O leitor precisa utilizar uma biblioteca pequena, bem mantida, ou outra solução
aprovada no marco da implementação; evitar criar um parser TOML completo.

Campos essenciais:

| Campo | Tipo | Regra |
|---|---|---|
| `schema_version` | inteiro | obrigatório, começa em `1`; incompatível => erro claro |
| `id`, `name` | string | obrigatórios; `id` estável |
| `description`, `author`, `license` | string | opcionais, não mudam a simulação |
| `seed` | inteiro sem sinal | obrigatório para modo procedural; configurável por CLI |
| `world.source` | enum | `authored`, `procedural`, `geographic` |
| `world.width`, `world.height` | inteiros | contagem de setores, positivos e limitada por configuração |
| `world.sector_size_m` | inteiro positivo | padrão compatível com `TERRITORY.md`: 2000 |
| `deployment.source` | enum | `authored` ou `procedural` |
| `factions` | lista | exatamente WEST e EAST no protótipo; extensibilidade posterior |
| `files.*` | caminho relativo | opcional; deve existir se declarado |

O esquema definitivo deve documentar também `time`, `victory`, `resources`,
`reinforcements`, `initial_intel` e opções de geração. Aplicar os defaults
**versionados** dos documentos responsáveis quando não houver override.
Valores numéricos devem ter unidade explícita em nome ou documentação; nunca
interpretar silenciosamente graus como metros.

### 4.1 Exemplo mínimo completo: mapa procedural e forças geradas

```toml
schema_version = 1
id = "rolling-hills"
name = "Rolling Hills"
description = "Fictional campaign generated from a fixed seed."
seed = 12345

[world]
source = "procedural"
width = 8
height = 8
sector_size_m = 2000

[world.generator]
terrain = "mixed"
water_fraction = 0.08
road_density = 0.35
settlement_count = 3

[deployment]
source = "procedural"
balance = "symmetric"   # equal budget, not identical equipment
west_budget = 100
east_budget = 100
```

Os números de geração são meramente ilustrativos; nenhum deles define tamanho
real de forças. O gerador e seus limites ainda precisam ser implementados.

### 4.2 Exemplo: geografia real, forças fictícias

```toml
schema_version = 1
id = "real-terrain-fictional-war"
name = "Real terrain, fictional war"
seed = 2026

[world]
source = "geographic"
width = 8
height = 8
sector_size_m = 2000

[world.geography]
origin_lat_deg = 47.8
origin_lon_deg = 11.3
projection = "local-metric"
dataset = "world/terrain-manifest.toml"

[deployment]
source = "authored"

[files]
units = "units.csv"
installations = "installations.csv"
```

As coordenadas e o sistema `local-metric` são somente ilustrativos. A
conversão cartográfica precisa registrar sua projeção e origem em metadados
válidos antes que distâncias, altitudes ou posições tenham significado.

### 4.3 Exemplo: cenário definido manualmente

```toml
schema_version = 1
id = "border-valley"
name = "Border Valley"
seed = 7001

[world]
source = "authored"
width = 4
height = 4
sector_size_m = 2000

[deployment]
source = "authored"

[files]
sectors = "world/sectors.csv"
units = "units.csv"
installations = "installations.csv"
```

Para um mundo autoral, permitir preenchimento de atributos ausentes com
defaults de terreno claramente documentados, **mas nunca inventar unidades**
quando `deployment.source = "authored"`.

## 5. CSV simples: setores, forças e instalações

Um CSV por entidade, com primeira linha de cabeçalho; UTF-8; delimitador
`,`; aspas duplas segundo RFC 4180; linhas vazias ignoradas; erros incluem
arquivo, linha, coluna e ID. Não usar CSV como substituto de relações
complexas sem necessidade; separar novas tabelas quando surgirem.

### 5.1 `world/sectors.csv`

```csv
x,y,terrain,traversable
0,0,plain,true
1,0,plain,true
2,0,forest,true
3,0,plain,true
```

Regra: ID interno do setor = `y * width + x`, conforme `TERRITORY.md`.
Initial controller and objective points belong to the military definition.
The current `OWGEO 2` loader derives this grid from the geographic raster. Para cenários 4×4, linhas não fornecidas
somente podem receber defaults quando `allow_partial_sectors = true` estiver
explicitamente habilitado; do contrário, requer cobertura completa.

### 5.2 `units.csv`

```csv
id,faction,type_id,sector_x,sector_y,count,formation_id
west-apache-1,WEST,AH-64A,0,0,2,west-air-1
west-armor-1,WEST,M1A1,1,0,4,west-ground-1
east-hind-1,EAST,MI-24V,3,3,2,east-air-1
east-armor-1,EAST,T-72B,2,3,4,east-ground-1
```

`id` identifica **lote de implantação**, não um veículo indivisível:
`count > 1` é expandido em entidades individuais ou formação conforme o
catálogo e as regras de `MILITARY_FORCES.md`, com IDs derivados de maneira
estável. Tipos precisam existir no catálogo versionado do projeto ou em um
catálogo de dados aprovado. O nome `MI-24V` é apenas uma chave de catálogo,
não uma suposição de seu desempenho.

### 5.3 `installations.csv`

```csv
id,type_id,sector_x,sector_y,operator,integrity
west-base-1,AIRBASE,0,0,WEST,1.0
east-refinery-1,REFINERY,3,3,EAST,0.8
```

O operador inicial não altera por si só a propriedade do setor. Recursos,
receitas, capacidade de reparo, estoques, guarnições e rede elétrica ficam nas
tabelas de economia/logística quando esses sistemas existirem. Neste anexo,
a definição de campos futuros **não os torna obrigatórios em M0**.

### 5.4 Facilidade de autoria

- Permitir cenários de uma página: `scenario.toml` com geração procedural.
- Permitir criação manual com três CSVs simples e uma descrição.
- Disponibilizar `examples/` oficiais, modelo vazio comentado e comando
  `--validate-scenario` com erros compreensíveis.
- Fornecer comando futuro `--export-scenario` para transformar um mundo
  gerado em arquivos editáveis, sem alterar o save da campanha.
- Para usuários avançados, aceitar edição de terreno e infraestruturas em
  arquivos externos; nenhum editor próprio é pré-requisito.

## 6. Geografia real: importação offline, não dependência do jogo

**Objetivo:** incorporar relevo e mapas de regiões reais, preservando a
simulação leve. A fonte pode conter DEM/GeoTIFF, dados vetoriais de
hidrografia/estradas/localidades e metadados de projeção/licença. A importação
é um **processo separado** que converte dados externos a um formato interno
simples; o jogo não precisa integrar GDAL, ferramentas GIS ou serviços online
para executar uma campanha já importada.

Fluxo:

1. Usuário obtém dados com direito de uso e informa região de interesse.
2. Ferramenta de importação verifica referência espacial, resolução,
   cobertura, NoData e atribuição/licença.
3. Reprojeta para coordenadas métricas locais e recorta a área definida.
4. Reduz/amostra relevo para heightmap em resolução configurável;
   distingue água, elevação e passabilidade.
5. Constrói setores `2 km` (default), atributos agregados e grafo de
   transporte; marca estradas e pontes apenas quando confirmadas pelos dados.
6. Exporta `WorldDefinition` com manifesto, proveniência, escala e checksums.
7. Usuário adiciona forças, objetivos, instalações e estoques por CSV/TOML,
   ou opta por gerá-los com `deployment.source = "procedural"`.

**Limites explícitos:** DEM não fornece automaticamente pontes, pistas,
refinarias, unidades militares ou dados históricos. Se camadas opcionais não
estiverem presentes, deixar ausentes ou permitir edição manual; **não inferir
instalações reais de feições vagas**. O nome de uma cidade real não comprova
existência de infraestrutura militar utilizável no cenário.

O mundo real pode ser representado estrategicamente por setores e
visualizado em 3D com heightmap mais fino: as duas resoluções são distintas.
Registrar origem `(lat, lon)`, CRS/projeção, datum vertical quando conhecido,
resolução e extensão do recorte. A ferramenta deve rejeitar inconsistências de
unidade/projeção; não calcular distâncias diretamente em graus.

**Fontes de dados:** exemplos possíveis incluem modelos de elevação públicos
(e.g., Copernicus DEM/SRTM conforme licença) e OpenStreetMap para vias e
localidades. Não vincular o formato do jogo a um fornecedor único. Verificar
licenças, atribuição e condições de redistribuição antes de distribuir mapas
pré-convertidos; dados geográficos não devem ser baixados silenciosamente.

## 7. Geração procedural reproduzível

### 7.1 Pipeline e contratos

1. Interpretar e validar parâmetros, seed e limites de memória.
2. Gerar a malha geográfica (elevação, água, terreno e passabilidade).
3. Construir setores e adjacência segundo `TERRITORY.md`.
4. Posicionar localidades e instalações sob restrições explícitas.
5. Criar grafo logístico (estradas, travessias e ligações válidas).
6. Definir posições iniciais, forças e estoques respeitando catálogos,
   capacidade e orçamentos selecionados.
7. Definir centros de comando, objetivos, reservas e condições de vitória.
8. Validar consistência e viabilidade básica; se falhar, rejeitar ou tentar
   novamente com sub-seed documentada, dentro de um limite fixo.
9. Gerar `WorldDefinition`/`ScenarioDefinition` resolvidos e gravar metadados
   suficientes para reprodução.

### 7.2 Parâmetros expostos ao usuário

- Extensão e tamanho dos setores; classe de terreno (plano, misto,
  montanhoso), fração de água, cobertura de vegetação e densidade de vias.
- Quantidade e classes de localidades, instalações e objetivos.
- Densidade de forças, orçamento relativo WEST/EAST, distribuição de tipos,
  reservas e reforços disponíveis, classes de equipamentos habilitadas.
- Postura estratégica inicial, linha de frente inicial ou zonas de implantação.
- Estoques, produção, logística e configuração das condições de vitória.
- Seed numérica reproduzível; opcionalmente uma seed gerada automaticamente,
  exibida e gravada para reproduzir o mundo.

### 7.3 Regras de plausibilidade e equilíbrio

- Não posicionar bases em setores intransitáveis ou isolados sem acesso
  apropriado; justificar exceções por tipo de instalação.
- Respeitar capacidade de depósitos, acesso logístico, combustível,
  munição, tripulações e manutenção ao alocar veículos.
- Não colocar tropas inimigas dentro da mesma base inicial sem que o cenário
  declare explicitamente uma disputa inicial.
- Garantir entrada e objetivos alcançáveis **pela topologia relevante**;
  verificar ilhas, travessias e gargalos de logística, não apenas distâncias.
- Orçamentos iguais **não** obrigam equipamentos idênticos, taxas de vitória
  iguais ou composição simétrica. Expor assimetria como parâmetro.
- Não garantir artificialmente uma vitória de cada facção. Testes de muitas
  seeds avaliam partidas inviáveis, impasses e dominância sistemática.
- Nunca corrigir um cenário inviável com teletransporte, reforços gratuitos ou
  mudança silenciosa de território.

### 7.4 RNG e versões

Uma seed de 64 bits inicia a geração. Para reduzir dependência da ordem de
chamadas, manter fluxos determinísticos separados por etapa (terreno, estradas,
instalações, forças). O mecanismo exato de derivação deve ser especificado e
testado antes da implementação; **não usar `std::hash` para persistência**, nem
exigir compatibilidade byte a byte entre bibliotecas padrão distintas sem
controlar o algoritmo RNG e a distribuição.

Persistir `generator_version`, `schema_version`, seed, parâmetros e, no início
da campanha, o **mundo já resolvido**. Mudanças no gerador não devem alterar
saves antigos. Regeração com mesma versão/configuração/seed tem de produzir
os mesmos IDs e a mesma definição canônica.

## 8. Inicialização e contratos com outros subsistemas

| Sistema | Entrada do cenário | Responsabilidade em execução |
|---|---|---|
| Território | setores, controlador inicial, objetivos, vizinhança | captura/disputa conforme `TERRITORY.md` |
| Economia | instalações, capacidades, receitas, estoques | produção, consumo e conservação |
| Logística | grafo de rotas, meios e cargas iniciais | transporte, atrasos, perdas |
| Forças | catálogo referenciado, lotes, formações, reservas | inventário persistente e reposição |
| Manutenção | condição inicial, equipes e capacidade | preparo, desgaste e reparos |
| Inteligência | observações iniciais explicitamente declaradas | conhecimento próprio, decaimento e novas observações |
| Operações | objetivos e ordens iniciais, autoridade do jogador | decisões e missões |
| Combate | condições iniciais e perfis abstratos | danos, moral, perdas e efeitos |
| Tático | heightmap/modelos opcionais | somente visualização e simulação próxima |

A definição do cenário pode conter informações verdadeiras; **cada facção
somente recebe informações iniciais declaradas em `initial_intel`** ou obtidas
por observações simuladas. Não preencher automaticamente seu `IntelStore` com
a implantação inimiga. UI e IA usam `FactionView`, não os arquivos brutos de
cenário depois da inicialização.

IDs internos (`SectorId`, `UnitId`, `InstallationId`) devem permanecer estáveis
nas cargas. IDs textuais de autor devem ser validados e mapeados
explicitamente; referências não resolvidas causam erro.

## 9. Validação de cenários e diagnósticos

O comando futuro `simulator --validate-scenario PATH` deverá realizar:

**Sintaxe/schema:** versão, tipos, campos obrigatórios, intervalos,
arquivos presentes, CSV válido, formato de números, strings UTF-8,
duplicações e enumerações conhecidas.

**Integridade referencial:** unidades usam tipos existentes; formações,
estoques, instalações, setores, rotas, objetivos e reservas referenciam IDs
válidos; nenhuma entidade tem ID duplicado; posições estão dentro do mundo.

**Topologia:** IDs de setor previstos; dimensões compatíveis; links de
grafo válidos; não declarar estradas ou travessias além da malha sem regra;
verificar componentes desconectados e sinalizar problemas de acesso.

**Viabilidade inicial:** duas facções válidas, objetivos declarados, dados
mínimos suficientes, bases compatíveis, orçamentos/estoques não negativos;
condições de vitória alcançáveis ou aviso de campanha sandbox deliberada.

**Segurança/desempenho:** limitar dimensões, quantidade de entidades, tamanho
por arquivo e profundidade de referências; não seguir caminhos arbitrários,
URLs ou scripts; recusar consumo excessivo de memória antes de alocar arrays.

Distinguir `ERROR` (impede iniciar) de `WARNING` (plausibilidade, equilíbrio,
dados opcionais) e fornecer mensagens acionáveis:

```text
ERROR scenarios/border-valley/units.csv:4: type_id 'X-99' is unknown.
ERROR scenarios/border-valley/world/sectors.csv:9: x=8 is outside 0..7.
WARNING scenarios/border-valley: east-refinery-1 has no transport link.
```

**Não** aplicar correções silenciosas a dados de autoria. Permitir
`--strict` para tratar avisos selecionados como erro, sem alterar a definição.

## 10. Interação do usuário e interface futura

Fluxo mínimo de linha de comando, quando essa funcionalidade for implementada:

```text
simulator --list-scenarios
simulator --validate-scenario scenarios/border-valley
simulator --scenario scenarios/border-valley --seed 12345
simulator --generate-scenario scenarios/procedural-hills --seed 12345
simulator --export-scenario output/editable-world
```

Essas linhas são **contratos propostos**, não comandos existentes. A UI futura
pode oferecer um assistente com mapa, tamanho, terreno, facções, orçamento,
estoques e seed. Deve sempre permitir inspecionar e editar os arquivos gerados.

Compartilhamento: copiar uma pasta de cenário ou um arquivo ZIP que contenha
apenas dados; verificar versão, licença de dados geográficos e caminhos ao
importar. Um ZIP nunca deve poder sobrescrever arquivos fora da pasta-alvo.

## 11. Save, replay e compatibilidade

Um save armazena estado resolvido (ou referência com hash imutável e cópia
segura), alterações da campanha, tick, estado de RNG, eventos, visões de
inteligência e estado de vitória. Carregar não depende de rede, GIS nem do
gerador procedural. A ordem de eventos segue `TERRITORY.md`.

Armazenar hash canônico de definição/arquivos e versão de regras. Se o pacote
de cenário original mudar, o save permanece válido com seu snapshot; avisar
sobre divergência para auditoria. Migrações futuras devem ser explícitas,
versionadas e testadas; incompatibilidade não pode reinterpretar campos
silenciosamente.

## 12. Historical milestones and scope limits

The following table records the earlier implementation proposal. The current
file contracts are documented in section 2.2 and the offline import guide.

| Marco | Entrega | Proibido antecipar |
|---|---|---|
| S0 (M0 atual) | Separação conceitual mundo/cenário/campanha; mundo pequeno explícito; seed e IDs estáveis; testes headless | Parser TOML/CSV, GIS, geração de forças, ferramentas de edição |
| S1 | Leitura/validação do manifesto e exemplo autoral mínimo; diagnósticos | Geografia real, gerador extenso |
| S2 | Import/export dos CSVs de setores, unidades e instalações conforme subsistemas disponíveis | Simular combate ainda não implementado |
| S3 | Gerador procedural básico, seed e validação, sem renderizador | Realismo cartográfico avançado |
| S4 | Importador geográfico offline e testes de CRS/escala; formatos de relevo para visual | Download automático e dependência de GIS no jogo |
| S5 | Seleção via interface e compartilhamento seguro de pacotes | Editor 3D como requisito |

A ordem S1–S5 depende da maturidade dos subsistemas. Não criar estruturas
abstratas e módulos vazios antecipadamente: interfaces e arquivos só quando
exigidos pelo marco autorizado.

## 13. Testes de aceitação (mínimos)

| ID | Teste | Resultado exigido |
|---|---|---|
| SC01 | Dois cenários autorais com mesmo mundo e diferentes forças | Compartilham geografia, mas inicializam inventários distintos |
| SC02 | Mesmo cenário procedural/seed/versão duas vezes | Mesmos setores, IDs, infraestrutura e lotes iniciais |
| SC03 | Mudança de seed | Pode alterar dados procedurais, sem modificar campos fixos |
| SC04 | Reabrir save procedural depois de atualizar gerador | Mesmo mundo e campanha do save, sem gerar novamente |
| SC05 | Unidade aponta para setor inexistente | Validação rejeita antes de iniciar |
| SC06 | Dois registros com mesmo `id` | Erro com local de origem |
| SC07 | Autor informa um tipo de veículo desconhecido | Erro, sem substituir por modelo parecido |
| SC08 | Cenário definido manualmente omite unidades | Nenhuma unidade inventada automaticamente |
| SC09 | Mundo geográfico sem camada de estradas | Não inventa rotas; avisa ou aceita conforme modo explícito |
| SC10 | Latitude/longitude tratadas como metros | Validação de CRS bloqueia interpretação incorreta |
| SC11 | Instalação sem ligação logística | Aviso de viabilidade; nenhum transporte mágico |
| SC12 | WEST observa EAST sem sensor/evento | Visão WEST não recebe unidades ou setores secretos |
| SC13 | Cenário com 90% do mapa da mesma facção | Não termina guerra automaticamente por área |
| SC14 | Pasta maliciosa com `../` ou ZIP traversal | Rejeitada sem acessar fora da raiz |
| SC15 | Tamanho de mundo acima do limite | Erro antecipado, sem alocação excessiva |
| SC16 | Cenário copiado para outro computador compatível | Executa sem recompilar e sem acesso à rede |
| SC17 | Duas execuções headless com mesma seed, cenário e comandos | Mesmos eventos e estado final canônico |
| SC18 | Rerun com mesmo mundo real e inventários procedurais | Relevo fixo; forças variam conforme seed |
| SC19 | CSV com aspas, Unicode e linhas inválidas | Interpretação válida quando possível; erros localizados |
| SC20 | Carregar save depois de modificar cenário externo | Snapshot preserva a campanha; aviso de divergência |

## 14. Definição de pronto e questões abertas

O sistema de cenários estará pronto quando um usuário puder criar uma pasta
com um manifesto e, se desejar, CSVs; validar erros; iniciar uma campanha
sem escrever C++; compartilhar o pacote; gerar uma campanha por seed;
utilizar um mundo convertido de dados geográficos reais; e retomar um save
sem dependência do gerador ou do pacote original.

**Decisões a fechar antes da implementação correspondente:** biblioteca/formato
definitivo de serialização, sintaxe de CSV para estoques e missões, projeção e
resolução de mapas reais, ferramentas de conversão aceitas, critérios e
parâmetros de geração de infraestrutura e forças, tamanho máximo por
hardware, licenças de dados distribuídos, perfis de balanceamento e conteúdo
mínimo das forças. Nenhuma dessas decisões bloqueia M0.

**Relação com os demais anexos:** este documento define *como criar as
condições iniciais*; não redefine captura, combate, produção, detecção,
logística ou vitória. `TERRITORY.md` permanece normativo para domínio e fim
da guerra; os anexos 02–09 permanecem normativos para cada subsistema.
