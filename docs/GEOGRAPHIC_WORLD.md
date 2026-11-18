# OpenWar — Geographic World and Movement Specification

**Status:** especificação de arquitetura e requisitos para implementação incremental  
**Revisão:** 1.0 — 18 de novembro de 2026  
**Escopo:** mundo geográfico, importação cartográfica, posicionamento, caminhos e mobilidade  
**Projeto:** OpenWar, motor determinístico de campanhas militares  
**Documentos relacionados:** `DESIGN.md`, `TERRITORY.md`, `SCENARIOS.md`, `CAMPAIGN_SETUP.md`, `OPERATIONS.md`, `LOGISTICS.md`, `COMBAT.md`, `TACTICAL.md`, `INTELLIGENCE.md` e `COMMAND_INTERFACE.md`.

> **Contrato principal:** o mundo geográfico contém posições e feições contínuas; os setores quadrados são um índice administrativo e estratégico. A grade não determina a geometria visual, a transitabilidade, a direção ou o custo de um deslocamento.

## 1. Objetivo e fronteiras do sistema

OpenWar deve aceitar mundos fictícios gerados proceduralmente e territórios derivados de dados geográficos reais, preservando um único motor de campanhas. O mesmo mundo alimenta o mapa operacional 2D, a simulação terrestre e logística e, posteriormente, um jogo tático 3D separado. A base geográfica **não** depende de renderização, motor de voo, shaders, malhas 3D nem de conexão com serviços externos durante uma campanha.

### 1.1 Requisitos normativos

- **GEO-001:** Toda entidade geograficamente implantada possui posição métrica autoritativa; o setor ocupado é derivado dessa posição, nunca a única localização.
- **GEO-002:** Coordenadas de estradas, pontes, rios, instalações e unidades compartilham o mesmo referencial espacial.
- **GEO-003:** Um setor pode conter simultaneamente estrada, floresta, rio, edificações e solo aberto; nenhuma categoria predominante substitui as geometrias originais.
- **GEO-004:** Duas travessias pelo mesmo setor podem ter custo, duração, consumo e restrições diferentes em função do caminho e da direção.
- **GEO-005:** Movimento terrestre pode combinar trechos por estrada e fora dela, com acesso legal e transição explícita.
- **GEO-006:** Atributos de mobilidade dependem de perfil de unidade/equipamento e condições dos trechos, não de um custo universal do setor.
- **GEO-007:** A atualização geográfica depende do relógio simulado, não do FPS, e permanece determinística entre execução contínua, em lotes e save/load.
- **GEO-008:** Mudança de setor é um fato de localização; **não** transfere automaticamente o controle territorial.
- **GEO-009:** A importação geográfica é reproduzível e offline na execução do jogo; seus dados resolvidos são preservados no mundo/salvamento.
- **GEO-010:** O estado autoritativo e as visões de inteligência por facção continuam separados.

### 1.2 Fora do escopo inicial

Física detalhada de suspensão/tração, dinâmica de helicópteros, balística, navegação marítima, modelos 3D de unidades, geração integral do cenário por IA, combate por linha de visão e atualização de mapas diretamente pela internet **não** são pré-requisitos da arquitetura. Esses sistemas poderão usar posições, altitudes e geometria quando forem implementados.

## 2. Relação com a arquitetura existente

The current implementation loads physical geography into `geography::Map`
and derives `WorldDefinition` as a strategic grid, including terrain summaries.
`OWGEO 2` is loadable on its own. Independent `OWSCENARIO 1` definitions
reference it and resolve deployments, objectives, and rules. Formations and
carriers move along metric routes. Fixed stockpile access points locate
installations, and mobile stocks follow a formation. Campaign state owns
territorial control and active journeys. Checkpoints embed the resolved world
and live state. The procedural sector generator and `TransportRoute` remain
available through the legacy path.

**Estratégia de evolução:** adicionar o mundo geográfico sem descartar imediatamente esses cenários e invariantes. O adaptador legado deve preservar os testes antigos, enquanto novos cenários geográficos adotam coordenadas, redes e movimento espacial. A troca da localização autoritativa dos subsistemas será feita explicitamente, não por campos duplicados inconsistentes.

### 2.1 Propriedade dos dados

| Camada | Responsabilidade | Mutável em campanha? |
|---|---|---|
| Geographic world (`geography::Map` and derived `WorldDefinition`) | Spatial frame, elevation, land cover, water, roads, bridges, extent, grid, and fixed geographic features | Reusable physical source |
| `ScenarioDefinition` | Facções, posicionamentos iniciais, instalações, estoques, objetivos e regras | Não, após iniciar |
| `CampaignState` | Posições atuais, controle, destruição de pontes, obstruções, missões, inteligência, logística e progressos | Sim, pelas regras |
| `FactionView` / snapshot autorizado | Informações e posições conhecidas por determinada facção | Derivação filtrada; nunca fonte de verdade |
| Frontend 2D/3D | Câmera, zoom, seleção, desenho e rascunhos de ordens | Não altera estado sem comando validado |

Uma ponte pertence à geometria do mundo; seu estado *intacta/destruída/bloqueada* pertence à campanha. O mesmo vale para condição de uma estrada, disponibilidade de um aeroporto e posições de unidades móveis.

The current engine stores road conditions in its private resolved map copy.
Separate campaigns can load the same source and change their own conditions
independently. The source world file remains reusable.

## 3. Sistema de coordenadas e precisão

### 3.1 Referencial local do mundo

Adotar um **plano métrico local ao teatro de operações**, com origem no canto noroeste da extensão retangular: `east_mm` aumenta para leste, `south_mm` aumenta para sul e `up_mm` aumenta em altitude relativa ao datum vertical definido. Inteiros com sinal de 64 bits em milímetros são os valores canônicos para armazenamento e cálculo determinístico; conversões para metros `double`/`float` ficam restritas à apresentação e a interfaces externas.

```cpp
struct MapPoint {
    std::int64_t east_mm;
    std::int64_t south_mm;
    std::int64_t up_mm;
};
```

Este tipo é **conceitual**: os nomes reais deverão respeitar a nomenclatura consolidada na implementação. O plano em milímetros não é um CRS geográfico: um importador deve projetar latitude/longitude num CRS métrico apropriado, escolher origem, transformações e datum de altitude e registrar esses metadados. Para o renderizador 3D, a transformação recomendada é `(east, up, south)` em metros, com origem visual deslocada para perto da câmera (*origin rebasing*) quando necessário.

### 3.2 Projeção cartográfica

- Para regiões reais, escolher projeção métrica adequada à extensão e latitude: UTM pode servir em uma zona; mapas que atravessam zonas ou cobrem extensões maiores precisam de outra escolha explícita.
- Registrar CRS de origem, CRS de destino, parâmetros, origem projetada, datum vertical, extensão, unidade e transformações efetivamente usadas.
- **Não** converter graus para metros por multiplicador fixo; **não** usar latitude/longitude como distâncias euclidianas.
- Aplicar uma transformação única a DEM, feições lineares, polígonos e pontos. Documentar erro esperado de projeção para o tamanho do teatro.
- Não presumir que alturas DEM e elevações de diferentes bases usem o mesmo referencial vertical; normalizar ou declarar incerteza.
- A instalação de bibliotecas GIS, se necessária, deve ficar preferencialmente na ferramenta de importação, não no runtime do motor.

### 3.3 Limites e identificação de setores

Com setor de lado `S` metros, mundo de largura `W` e altura `H`, a associação de uma posição válida é:

```text
column = floor(east_mm  / (S * 1000))
row    = floor(south_mm / (S * 1000))
sector_id = row * W + column
```

Setores usam intervalos semiabertos `[min, max)`. Uma posição exatamente a 2.000 m do limite oeste pertence à próxima coluna. Os limites leste e sul da extensão são externos ao mundo. Coordenadas negativas ou fora da extensão são rejeitadas ou processadas por uma regra explícita de entrada/saída do teatro, nunca truncadas silenciosamente. Evitar divisão inteira de negativos sem ajuste, pois C++ trunca em direção a zero; verificar limites antes do índice.

O `SectorId` não deve ser um campo independente e mutável que possa divergir da posição: pode existir como índice/cache invalidável e recomputável. Atribuir posição exige validação de limites e compatibilidade com o tipo da entidade.

## 4. Geografia e camadas de dados

### 4.1 Camadas mínimas

| Camada | Representação autoritativa | Usos |
|---|---|---|
| Relevo | Amostras de elevação georreferenciadas/tiles e metadados verticais | Declividade, altitude, terreno 2D/3D posterior |
| Cobertura/uso do solo | Raster categórico ou polígonos, com códigos normalizados | Terreno fora de estrada, mapa visual, possível ocultação futura |
| Hidrografia | Linhas, polígonos e barreiras de travessia | Lagos, margens, rios, vau, ponte |
| Estradas/trilhas | Grafo geométrico de segmentos e nós, com polilinhas | Transporte, marchas, acesso a instalações |
| Ferrovia | Rede própria, se suportada pelo cenário | Transporte ferroviário futuro |
| Povoações/edificações | Polígonos, pontos e atributos | Mapa, objetivos e instalações |
| Pontos de interesse | Posição e ID estáveis | Pontes, portos, bases, depósitos, cruzamentos |
| Setores | Malha sobreposta e dados agregados | Controle, inteligência, objetivos e relatórios |

Um atributo agregado como `forest_fraction` ou `dominant_terrain` é útil para resumo, mas **nunca substitui** os dados que o produziram. Os visuais podem desenhar a floresta irregular e o rio curvo; não se deve renderizar a malha como blocos de 2×2 km.

### 4.2 Resolução e detalhe

O tamanho do setor estratégico (`sector_size_m`) e a resolução do raster de mobilidade são parâmetros independentes. O protótipo adota 100 m para um mapa pequeno; **100 m não é constante do motor**. Dados finos são armazenados por tiles/indexação regional. Um mundo de 256×256 km a 100 m por célula teria 2.560×2.560 = 6.553.600 células, antes das demais camadas. Isso exige orçamento de memória, leitura e roteamento por região, não alocação compulsória de grafo explícito por célula.

Pequenas feições críticas (rios estreitos, muros, valas, pontes, túneis) precisam sobreviver à rasterização por **geometrias de barreira/conexão explícitas**. Reduzir um rio a uma célula de solo acessível por média de cobertura não pode criar uma passagem inexistente.

## 5. Rede rodoviária e travessias

### 5.1 Modelo mínimo

```text
RoadNode: id, MapPoint, tipo de conexão
RoadSegment: id, node_a, node_b, polyline ordenada,
             categoria/superfície, limite de velocidade,
             sentido permitido, restrições de acesso,
             estado físico e capacidade por travessia
```

- Um segmento é percorrido geometricamente pela sua polilinha, não pela distância entre os centros de setores.
- Um cruzamento gráfico de duas linhas só conecta rotas se existir **junção topológica** declarada. Viadutos e passagens inferiores não conectam por coincidência em projeção 2D.
- Travessias de pontes devem registrar o obstáculo atravessado, capacidade e elegibilidade; uma ponte bloqueada não torna automaticamente atravessável a água abaixo.
- Direções são explícitas: mão única, restrição de conversão ou fechamento em um sentido precisam aparecer no grafo.
- Pontos de entrada e saída de estrada são legais onde a geometria e o perfil de mobilidade permitirem; não apenas nos nós cadastrados.
- As capacidades e fechamentos dinâmicos pertencem ao `CampaignState` e são aplicados ao grafo estático sem reescrever geometrias.

### 5.2 Custo direcional

Para um trecho, o custo deve considerar comprimento percorrido, classe/superfície, velocidade nominal do equipamento, limite da via, declividade **com sinal no sentido percorrido**, capacidade de tração, massa/limite de ponte, restrições e condições dinâmicas. Resultados mínimos: tempo e combustível; futuramente, desgaste e risco podem ser contabilizados por políticas separadas.

**Exemplo:** o mesmo segmento é percorrível de oeste para leste a 5 m/s em aclive e no retorno a 10 m/s em declive. Isso é um atributo da travessia direcionada, não do setor. Uma unidade pode gastar mais tempo para cortar um setor perpendicularmente à estrada do que para seguir pela estrada em diagonal por vários setores.

## 6. Terreno fora de estrada

### 6.1 Perfil de mobilidade

Cada tipo relevante de deslocamento recebe um perfil reutilizável, sem criar física de veículo:

```text
MobilityProfile:
    classe: a pé / rodas / lagartas / outras
    velocidade em terreno nominal
    velocidades/penalidades por cobertura e solo
    limite de aclive/declive
    restrições de água, largura, peso, ponte, superfície
    taxas de consumo em estrada e fora de estrada
```

Uma formação adota, por regra explicitamente definida, a velocidade/limites do equipamento relevante mais restritivo; equipamentos transportados ou deixados na retaguarda exigem política própria, sem pressupor que todo o inventário viaja junto.

### 6.2 Células e barreiras

- A malha de mobilidade fina consulta solo, vegetação, altura e restrições para gerar arestas locais sob demanda.
- Conectividade ortogonal ou 8-vizinhos pode ser usada inicialmente; diagonais que cruzem cantos bloqueados são rejeitadas.
- Arestas devem consultar obstáculos geométricos atravessados; uma célula média 'plana' não autoriza cruzar um canal ou cerca intransponível.
- A inclinação é computada de maneira consistente a partir de altura e distância horizontal; em terrenos muito inclinados o caminho pode ser rejeitado para rodas e aceito para pedestres.
- A resolução determina o detalhe da navegação, mas não altera a extensão física dos setores.

### 6.3 Rotas híbridas

Uma rota pode ter `off-road connector → estrada → ponte → estrada → off-road connector`, escolhendo acessos viáveis, inclusive mais distantes do que a estrada aparentemente mais próxima. A entrada em trecho interior da via deve gerar acesso temporário geométrico para a consulta, sem corromper IDs ou a topologia permanente. Todos os segmentos e conectores são avaliados pelo mesmo perfil de mobilidade.

## 7. Busca de caminhos e limites de desempenho

**Primeiro algoritmo:** Dijkstra determinístico sobre grafo combinado (estradas e ligações locais fora de estrada), com ordenação estável e desempates documentados por tempo, combustível e IDs de segmentos. A implementação deve distinguir entre *menor tempo*, *menor combustível* e *menor tempo sob orçamento de combustível*. Executar duas buscas simples (tempo e combustível) **não garante** encontrar o trajeto mais rápido entre todos os trajetos viáveis com um orçamento intermediário; registrar essa limitação e incorporar busca multi-critério quando os cenários a exigirem.

- Cada consulta fornece origem e destino reais, perfil, restrições de facção e visão de inteligência, além de orçamento aplicável.
- Conhecimento limitado afeta **planejamento**; obstáculos verdadeiros afetam **execução**. O planejador não pode se beneficiar de unidades ou estradas ocultas ao operador.
- `PathResult` retorna sequência geométrica, segmentos, tempos, combustível, setorização atravessada, motivo de recusa e dados necessários ao replanejamento.
- Indexar espacialmente nós/segmentos e tiles para evitar percorrer todo o mundo em cada consulta.
- Limitar regiões de busca quando justificável; uma heurística de limite **não pode** declarar rota inexistente sem distinguir 'não encontrada no orçamento de busca' de 'comprovadamente impossível'.
- Medir custo por perfil, quantidade de unidades e concorrência de missões; não extrapolar benchmarks atuais de 128×128 setores, feitos com roteamento abstrato, para o motor geográfico ainda não integrado.

## 8. Execução do movimento

### 8.1 Estado de viagem

```text
Journey:
    entity_id, origin_point, destination_point
    geometry_version / route_revision
    legs[] (tipo, polilinha/identificador, duração e custo resolvidos)
    active_leg, elapsed_microseconds, current_point
    committed_fuel_debits / progress accounting
    status: moving / paused / blocked / arrived / cancelled
```

O estado acima é conceitual. **A posição atual é única**: pode ser computada da perna e progresso, mas o contrato precisa especificar como materializar a posição exata ao interromper e salvar uma viagem. Tempo de simulação inteiro (microsegundos ou outra unidade fixa) e interpolação de pontos fixos evitam dependência do renderizador.

### 8.2 Regras temporais e de recursos

- Um tick pode atravessar mais de uma perna, usando o tempo restante após o waypoint; não perder frações de progresso em trocas de segmento.
- Política inicial de combustível: débito por perna em entrada, no máximo uma vez; salvar explicitamente débitos comprometidos. Limitações e alternativas (consumo progressivo) devem ser documentadas, não misturadas silenciosamente.
- O fechamento de uma rota antes de entrar nela interrompe ou força replanejamento desde a **posição atual**, sem teletransporte.
- Em deslocamento, unidades e transportadores atravessam setores e podem gerar eventos ordenados de passagem com tempos dentro do tick.
- O caminho, custos e estado devem sobreviver a save/load sem execução novamente do gerador nem pagamento duplo.
- O mapa 2D pode interpolar para desenhar fluidez, mas nunca determinar a posição real da simulação.

### 8.3 Integração por subsistema

**Militar:** ordens podem manter `SectorId` para objetivos estratégicos, mas devem ser resolvidas em *pontos geográficos relevantes*: ponte, cruzamento, instalação, ponto de reunião ou coordenada. Entrada no setor e captura são eventos distintos. Combate segue inicialmente abstrato no setor, sem inventar geometria de alvo oculto.

**Logística:** a coleta e entrega exigem chegada ao **ponto de acesso do depósito**; estar no mesmo setor não basta. Entregas para formações móveis replanejam desde a posição atual; estoque, reserva, capacidade e carga seguem o modelo econômico existente. O fechamento de uma ponte afeta rotas, não a contabilidade do combustível.

**Infraestrutura e instalações:** as posições são fixas, vinculadas aos estoques como no código atual, salvo alteração explícita de modelo. Um depósito transportado por formação acompanha sua unidade com um único vínculo canônico.

**IA:** consulta rotas e dados conhecidos, sem acessar posição real do inimigo. O aumento da resolução espacial pode demandar novas políticas de aproximação, mas não altera a autoridade do `CampaignState`.

**Combate e inteligência:** localização precisa não autoriza automaticamente identificar alvos dentro do mesmo setor. Informações observadas têm posição estimada, resolução, tempo e confiança; posições exatas próprias e inimigas não têm o mesmo regime de visibilidade.

## 9. Importação de geografia real

### 9.1 Pipeline offline

```text
Fontes (OSM, DEM e cobertura terrestre)
  -> download externo / arquivo já adquirido
  -> conferência de origem, versão e licença
  -> recorte da área + reprojeção métrica
  -> normalização de classes e elevação
  -> topologia de vias e travessias
  -> barreiras/raster de mobilidade/tiles
  -> setorizações e pontos de interesse
  -> validação de alcançabilidade + qualidade
  -> WorldDefinition resolvido e pacote versionado
```

O runtime não deve depender de chamadas a OSM, Copernicus ou provedores de tiles. Dados importados têm proveniência com URLs, datas, versões, licenças, escala, CRS e transformações. Erros de dados não são mascarados: ausência de ponte, junção ambígua, geometria inválida ou DEM incompleto geram diagnóstico rastreável.

### 9.2 Fontes e condições de uso

- **OpenStreetMap** para vias, hidrografia, localidades e polígonos mapeados. A cobertura varia por região; OSM não é garantia de exaustividade nem de classificação militar. Dados OSM são sob ODbL, com atribuição aos contribuidores e obrigações adicionais possíveis para bases derivadas compartilhadas. Ver https://www.openstreetmap.org/copyright e https://osmfoundation.org/wiki/Licence/Attribution_Guidelines.
- **Copernicus DEM** como uma possível fonte de altitude; a disponibilidade e as condições de uso mudam por produto, resolução e canal de acesso. Ver https://documentation.dataspace.copernicus.eu/APIs/SentinelHub/Data/DEM.html e as licenças aplicáveis. Não assumir acesso irrestrito à variante GLO-30 em qualquer API.
- **Cobertura vegetal/uso do solo:** fonte escolhida e classes precisam ter autoria/licença/resolução registradas. OSM sozinho pode não fornecer cobertura contínua adequada à mobilidade.
- **Imagens satelitais e tiles de visualização:** licenças separadas daquelas dos dados vetoriais; não inserir automaticamente conteúdo de serviços comerciais no pacote distribuível.

### 9.3 Processamento e validações

Conferir continuidade de estradas, conexões por nível, sentido, geometrias duplicadas, interseções realmente conectadas, pontes/rios, unidades de altura, valores nulos de raster, polígonos inválidos, pontos fora da extensão e posicionamento sobre terreno ilegal. O resultado deve poder ser visualizado em ferramenta de inspeção 2D independente do motor de combate.

## 10. Conteúdo procedural e legado

A geração procedural existente deve permanecer utilizável. Um adaptador legado coloca unidades e instalações em âncoras determinísticas (por exemplo, centro do setor quando legal), transforma enlaces atuais em corredores geométricos dirigidos e representa travessias de água por pontes identificadas. Esse adaptador **não comprova realismo geográfico**: é uma etapa de compatibilidade para comparar resultados.

Current geographic scenarios use explicitly loaded worlds. The procedural
sector generator remains available as a compatibility fixture. Generating
military deployments and objectives on a loaded geographic world is a later
feature.

Uma nova versão de gerador pode construir rios curvos, estradas sinuosas e distribuição de solo em resolução fina e, então, produzir setores de 2 km apenas como sobreposição. Os parâmetros geográficos, seed, versão do gerador, algoritmo e dados resolvidos devem ser registrados. Manter testes do mundo legado mesmo depois de adotar novos caminhos.

## 11. Persistência e compatibilidade de regras

- O M8 já possui formato binário com `schema_version` e `rules_version`; o movimento geográfico autoritativo exige versão apropriada e testes de migração.
- Persistir mundo geográfico **resolvido**, posições, deslocamentos parciais, rotas, reservas e combustíveis debitados, fechamentos de vias e dados necessários à retomada fiel.
- Reconstituir caches/índices derivados após carregar, sem alterar resultados ou sequências de eventos.
- Saves de regras antigas sem trajetórias precisas não possuem coordenadas para retomar uma viagem interna ao setor. Não afirmar compatibilidade automática: oferecer loader legado sob regras antigas ou conversão explícita com limites documentados.
- `save → load → continue` e `run contínuo` devem produzir posições, eventos, recursos e resultados idênticos sob mesmas regras e dados.

## 12. Visualização estratégica e futura visualização 3D

A UI 2D mostra geografia contínua, símbolos nas posições conhecidas e setores somente como overlay opcional. Pode desenhar unidades no interior dos setores e traçar rotas por vias reais. O visualizador **não** move unidades.

O eventual produto 3D usa os mesmos pontos geográficos e ativos de terreno mais detalhados, com transformação local para precisão de câmera; objetos 3D, animação de rotor, física do helicóptero e streaming visual são responsabilidade do **jogo tático**, não do motor de campanha. Uma câmera 3D de inspeção no OpenWar permanece opção futura, sem fazer parte dos critérios iniciais.

## 13. Protótipo obrigatório e aceitação

**Cenário de teste:** mundo headless de 3×2 setores, cada setor com 2.000 m; raster inicial de 100 m; rio norte–sul no intervalo horizontal de 1.900 a 2.100 m; estrada leste–oeste em `south=800 m` atravessando o rio por ponte. Origem `(1300,1000)` e destino `(2700,1000)` em metros, com acessos ortogonais à estrada.

| Código | Verificação | Resultado esperado |
|---|---|---|
| GEO-T01 | Rota mista | 200 m fora de via + 1.400 m na estrada + 200 m fora de via |
| GEO-T02 | Tempo e consumo | A 5 m/s fora e 10 m/s na via: 220 s. Com 20 L/km fora e 10 L/km na via: 22 L |
| GEO-T03 | Progresso | Aos 60 s: `(1500,800)`; aos 120 s: `(2100,800)` |
| GEO-T04 | Fronteira | Ao cruzar `east=2000 m` em 110 s, setor derivado muda; controlador não muda |
| GEO-T05 | Direção | Trecho com aclive/declive assimétrico produz custos diferentes na ida/volta |
| GEO-T06 | Ponte | Fechamento da única ponte torna a rota inviável |
| GEO-T07 | Acessos | Via próxima isolada não bloqueia escolha de acesso mais distante viável |
| GEO-T08 | Perfil | Transportador road-only rejeita acesso fora da via; formação habilitada pode usar |
| GEO-T09 | Batch/continuação | Quatro ticks juntos, separados e via checkpoint geram posições e eventos idênticos |
| GEO-T10 | Logística | Estar no mesmo setor do depósito não permite descarregar sem alcançar seu ponto |
| GEO-T11 | Extrapolação | Origem/destino no limite ou fora do mundo são tratados conforme contrato, sem overflow |
| GEO-T12 | Replanejamento | Fechamento no meio de viagem conserva posição, carga e recursos já debitados |
| GEO-T13 | Sem vazamento | Planejador da facção não usa posição/obstáculo inimigo oculto sem observação |

**Nota de precisão:** os resultados dos testes em pontos de passagem assumem geometria de rota exatamente definida na fixture. A ordem dos eventos dentro de um tick, arredondamento temporal e desempates devem ser documentados e fixados na implementação.

## 14. Historical implementation stages

The movement core, geographic ground and logistics routing, offline import,
regional routing and 2D viewer are implemented. Geographic world and military
scenario files can be loaded separately. Operational UI orders, APP-6, and
generation of military deployments on imported worlds remain planned.

1. **Geo-P0 — Núcleo isolado:** `MapPoint`, limites, CRS interno documentado, segmentos, terreno fino, perfis de mobilidade, rotas mistas e fixture de 3×2. Nenhuma mudança no motor legado.
2. **Geo-P1 — Mundo geográfico e adaptador:** versão do formato, mundo autoral procedural geográfico e âncoras estáveis; mapa debug 2D apenas para inspeção.
3. **Geo-P2 — Movimento militar:** posições autoritativas, ordens com destinos geográficos, planejamento e travessias; preservar ocupação e combate abstratos.
4. **Geo-P3 — Logística:** transportadores e depósitos com pontos reais, acesso, replanejamento de entregas móveis e limites de via.
5. **Geo-P4 — Importação offline real:** OSM/DEM, CRS, topologia, metadados de licença, validação e cenários pequenos reais.
6. **Geo-P5 — Integração operacional:** símbolos, rotas geográficas, informações por facção, controles e observabilidade no frontend 2D.
7. **Geo-P6 — Escala:** perfil de milhares de setores, tiles, caches comprovadamente necessários e regressões de determinismo.

Não iniciar Geo-P2, Geo-P3 nem a importação completa como efeito colateral de Geo-P0. Cada etapa conserva executáveis headless, snapshots reproduzíveis e conjuntos de testes independentes.

## 15. Critérios de conclusão

O sistema geográfico estará apto ao mapa operacional quando: posições de unidades e locais forem únicas e coerentes; rotas mistas e direcionais forem reproduzíveis; rio e pontes funcionarem sem 'efeito Minecraft'; as categorias dos setores forem derivadas, não substitutas da geografia; nenhuma ordem usar inteligência oculta; persistência restaurar o movimento sem diferenças; e houver API de consulta para o frontend sem acoplar simulação ao renderer.

A **importação geográfica real** estará concluída somente quando ao menos um recorte público real, com fonte/CRS/licença documentados, puder ser transformado offline em mundo validado e apresentado de maneira coerente, sem posicionamento arbitrário de pontes, vias ou instalações.

## 16. Referências

- Código e documentação OpenWar: https://github.com/lucianobianc/openwar
- OpenStreetMap, direitos e licença: https://www.openstreetmap.org/copyright
- OpenStreetMap Foundation, atribuição: https://osmfoundation.org/wiki/Licence/Attribution_Guidelines
- Copernicus Data Space, DEM: https://documentation.dataspace.copernicus.eu/APIs/SentinelHub/Data/DEM.html
- Especificação de interface complementar: `COMMAND_INTERFACE.md`

**Política de manutenção:** este anexo estabelece o alvo arquitetural; a documentação de código implementado (`README.md` e contratos atuais) continua descrevendo o estado executável até que cada etapa esteja concluída. Qualquer divergência exige decisão explícita e atualização coordenada dos anexos; não se deve alterar regras existentes silenciosamente.
