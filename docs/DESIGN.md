# OpenWar campaign architecture and helicopter game concept

DOCUMENTO DE CONCEPÇÃO

Visão consolidada, arquitetura funcional, economia de guerra e ordem de batalha inicial WEST × EAST.

Versão 0.1 · 17 de novembro de 2026 · Documento de trabalho. “Definido” indica decisões tomadas na conversa; “Proposto” identifica especificações iniciais para validação.

## 1. Visão e pilares do projeto

OpenWar is an autonomous, persistent campaign engine with two applications.
The standalone simulator presents an ongoing war in a geographic 2D command
interface. A future arcade helicopter game can reuse the engine and that
interface for briefings, planning, and mission outcomes. The engine and
standalone application operate independently of tactical rendering and flight
physics. The table below retains the original product pillars.

| **Pilar**              | **Decisão consolidada**                                                                                        |
|------------------------|----------------------------------------------------------------------------------------------------------------|
| Campanha autônoma      | Duas facções executam operações, disputam objetivos, perdem unidades e administram recursos finitos.           |
| Causalidade            | Destruição, conquistas, desgaste, suprimentos e inteligência alteram o curso da guerra.                        |
| Intervenção do jogador | Mapa 2D permite observar a guerra e interferir nas ordens, prioridades e missões de sua facção.                |
| Combate arcade         | Helicóptero jogável, inicialmente com HUD simples; cockpit detalhado fora do escopo inicial.                   |
| Desempenho             | Alvo: notebook i5-1135G7, Intel Iris Xe, 8 GB RAM; renderização low-poly e simulação escalonada.               |
| Evolução do escopo     | Tanques participam da guerra desde o início como entidades de IA; controle direto de tanques fica para depois. |

## 2. Separação das simulações

Um único estado autoritativo da campanha será compartilhado por módulos independentes. As diferenças entre níveis são de fidelidade e frequência de atualização; não há guerras paralelas ou duplicação de recursos.

| **Nível**   | **Responsabilidade**                                                                         | **Tratamento**                                                        |
|-------------|----------------------------------------------------------------------------------------------|-----------------------------------------------------------------------|
| Estratégico | Economia, produção, reservas, objetivos, comando, manutenção, moral e inteligência.          | Atualização em intervalos de tempo simulado; executável sem gráficos. |
| Operacional | Movimentos distantes, rotas, comboios, execução de missões e resolução abstrata de combates. | Entidades agregadas; baixa frequência.                                |
| Tático 3D   | Helicóptero do jogador, unidades próximas, sensores, projéteis, danos e colisões.            | Física e renderização apenas na região ativa.                         |

Na entrada da região tática, entidades estratégicas são materializadas em 3D com posição, ordens, danos, munição, combustível e identidade preservados. Ao sair, seu estado retorna à representação simplificada. Transições precisam evitar duplicação de entidades e resultados contraditórios.

Frequências indicativas, não especificações finais: voo a ~60 Hz; combate próximo a 10–30 Hz; operacional por segundos; logística por minutos simulados; comando estratégico em intervalos de vários minutos. Ajustar por perfilamento.

## 3. Mundo, território e infraestrutura

A geographic world describes the physical theater. Imported or explicitly
authored geography supplies its terrain, elevation, road network, water, and
locations. The strategic grid is derived from the theater and aggregates
territorial control, objectives, and intelligence. Military scenario files
reference the world and set initial deployments and rules. Campaign state owns
live positions, resources, control, and events. Resolved state is preserved in
checkpoints. `OWGEO 2` and `OWSCENARIO 1` are the current independent inputs.

O teatro de operações conterá bases aéreas, instalações industriais, centros de comando, depósitos, vias de transporte, posições defensivas, radares e objetivos capturáveis. Uma infraestrutura pode ser conquistada, danificada, reparada, ficar sem insumos ou ser temporariamente inutilizada.

| **Instalação**                     | **Efeito na campanha**                                                                         |
|------------------------------------|------------------------------------------------------------------------------------------------|
| Campo petrolífero / refinaria      | Extração e conversão de petróleo em combustível, dependentes de insumos, energia e capacidade. |
| Usina elétrica                     | Fornece energia a instalações; interrupções prejudicam produção e recuperação.                 |
| Fábrica / oficina industrial       | Gera materiais e componentes genéricos; não fabrica automaticamente helicópteros modernos.     |
| Oficina militar                    | Repara equipamentos, limitada por mão de obra, vagas e peças.                                  |
| Depósito                           | Armazena combustível, munição, peças e demais suprimentos.                                     |
| Base aérea                         | Opera helicópteros; possui capacidade de manutenção, preparação e estoque local.               |
| Porto / estação / centro logístico | Permite entrada de reforços e distribuição de suprimentos.                                     |
| Radar / centro de comunicações     | Gera observações e compartilha informações para a rede de defesa e o comando.                  |

A perda de uma refinaria não elimina o combustível já existente: o efeito emerge pela redução da produção e pelo esgotamento progressivo dos estoques. Instalações capturadas podem exigir reparo, pessoal, energia e tempo para voltar a funcionar.

## 4. Recursos, logística e manutenção

### 4.1. Recursos e fluxos

Recursos iniciais: combustível, munição por classes, peças/componentes, materiais industriais, equipamentos de reposição, pessoal e suprimentos gerais. Estoques são limitados; não há reabastecimento gratuito ou instantâneo.

A logística existe fisicamente no mapa: depósitos fornecem cargas a comboios, que seguem por rotas e podem atrasar, desviar, sofrer emboscadas ou ser destruídos. Unidades e bases requisitam itens conforme consumo previsto e prioridade operacional. Escoltas e ataques logísticos surgem como missões naturais.

### 4.2. Disponibilidade individual das aeronaves

| **Estado da aeronave** | **Consequência**                                                            |
|------------------------|-----------------------------------------------------------------------------|
| Disponível             | Pode ser alocada a uma missão.                                              |
| Preparação             | Inspeção, abastecimento e configuração de armamentos.                       |
| Em missão              | Consome combustível, munição, horas de voo e disponibilidade da tripulação. |
| Manutenção             | Inspeção programada ou intervenção corretiva por desgaste.                  |
| Reparo                 | Restauração após danos de combate.                                          |
| Aguardando recursos    | Indisponível por falta de peças, equipe ou capacidade de oficina.           |
| Perdida                | Baixa permanente no inventário; substituição apenas por reserva ou reforço. |

Tempo de retorno = espera em fila + tempo efetivo de trabalho + preparação final. A manutenção compete por equipes, ferramentas, vagas e peças; tripulações também acumulam fadiga e precisam descansar. O sistema deve registrar horas de voo, integridade e histórico da aeronave.

## 5. IA de comando, inteligência e comportamento

### 5.1. Hierarquia de decisões

Comando estratégico escolhe objetivos e aloca recursos; comando operacional organiza ofensivas, defesas, reconhecimento, transporte e suporte; agentes táticos executam ordens, identificam ameaças e decidem reagir, prosseguir, recuar ou pedir reforços. Preferências doutrinárias influenciam, mas não predeterminam, escolhas.

### 5.2. Informação imperfeita

O simulador detém o estado real do mundo. Cada facção mantém somente seu conhecimento: contatos observados, classe estimada, posição estimada, hora da observação, fonte e confiança. Informações envelhecem, podem estar erradas e precisam de reconhecimento ou confirmação. O mapa do jogador apresenta a visão de sua facção, sem revelar posições ocultas.

### 5.3. Moral, coesão, fadiga e experiência

| **Variável** | **Interpretação / efeitos**                                                               |
|--------------|-------------------------------------------------------------------------------------------|
| Moral        | Disposição de sustentar missões, aceitar risco, resistir à pressão ou solicitar retirada. |
| Coesão       | Capacidade de obedecer, coordenar e executar retirada organizada.                         |
| Fadiga       | Deterioração de desempenho e disponibilidade em operações prolongadas.                    |
| Experiência  | Influência em reação, detecção, execução e tolerância à pressão.                          |

Derrotas, isolamento, perdas, falta de suprimentos e cansaço deterioram condições; vitórias, descanso, reforços e liderança podem recuperá-las. Unidades não devem combater automaticamente até a destruição: podem pedir apoio, recuar, reorganizar-se ou render-se. Evitar limiares artificiais que produzam comportamento abrupto.

## 6. Interface de comando e criação de missões

The 2D viewer currently supports observation and inspection. The CLI can issue
the orders implemented by the campaign engine. The operational command interface
below is the intended product.

O mapa estratégico 2D é interativo e funciona como centro de operações, não apenas observação. O jogador acompanha movimentos, objetivos, estoques, inteligência e operações planejadas pela IA; também pode criar missões, alterar prioridades, determinar alvos, cancelar ordens e redistribuir meios sob sua autoridade.

| **Missão inicial**   | **Exemplo de finalidade**                                                         |
|----------------------|-----------------------------------------------------------------------------------|
| Reconhecimento       | Atualizar informações sobre rotas, forças e posições de defesa aérea.             |
| Ataque               | Interditar uma coluna, atingir um alvo militar confirmado ou apoiar uma ofensiva. |
| Escolta              | Proteger comboios ou aeronaves de transporte.                                     |
| Patrulha armada      | Vigiar e responder a ameaças dentro de área designada.                            |
| Apoio próximo        | Ajudar forças terrestres engajadas.                                               |
| Interdição logística | Prejudicar o abastecimento inimigo sem depender de destruição completa da força.  |

Antes de aceitar o planejamento, o sistema verifica aeronaves disponíveis, tripulações, combustível, armamentos, informações de alvo, base de partida, risco esperado e conflito com outras missões. Ordens podem ser programadas para execução posterior; a IA continua conduzindo o restante da guerra. O nível exato de autoridade do jogador ainda precisa ser definido.

## 7. Reforços, reservas e evolução da guerra

A campanha começa com inventários limitados e instalações econômicas operacionais ou danificadas. Indústrias locais fornecem combustíveis, materiais e peças genéricas; equipamentos militares complexos chegam principalmente por reservas e reforços externos. Reforços podem mudar a composição da guerra no decorrer dos dias, com novas aeronaves ou unidades.

O comando superior pode liberar reforços segundo necessidade, disponibilidade, perdas e importância estratégica, com atraso de chegada e exigências logísticas. Sem criação espontânea de veículos: reforços devem ter origem, quantidade, custo de oportunidade e janela de chegada.

## 8. Identidade das facções e período de referência

Duas facções fictícias: WEST e EAST. Referência visual e tecnológica aproximada: final da Guerra Fria / 1990–1995. Equipamentos e variantes devem ser selecionados de modo coerente com o ano exato da campanha; a lista a seguir é um catálogo proposto, não uma ordem de batalha fechada.

| **Dimensão**         | **WEST — proposta**                                                                | **EAST — proposta**                                                                           |
|----------------------|------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------|
| Doutrina operacional | Reconhecimento, precisão, coordenação de armas combinadas e preservação dos meios. | Concentração de forças, artilharia, defesa em camadas e operações mecanizadas.                |
| Ponto de atenção     | Dependência de manutenção, logística e peças especializadas.                       | Dependência de apoio logístico e coordenação; estoques e quantidades definem capacidade real. |
| Comportamento da IA  | Tendências configuráveis por comandante e contexto, nunca bônus fixos.             | Idem; comandantes podem adotar posturas diferentes da tendência doutrinária.                  |

## 9. Catálogo inicial WEST

| **Categoria**            | **Equipamentos candidatos**                                  | **Papel no sistema**                                                    |
|--------------------------|--------------------------------------------------------------|-------------------------------------------------------------------------|
| Helicópteros de ataque   | AH-64A Apache; AH-1F Cobra (opcional)                        | Ataque anticarro, escolta e interdição.                                 |
| Helicópteros utilitários | UH-60 Black Hawk; CH-47 Chinook (futuro)                     | Transporte, movimentação de pessoal e suprimentos.                      |
| Carros de combate        | M1A1 Abrams                                                  | Força blindada de choque; consumo de combustível, manutenção e munição. |
| Blindados de infantaria  | M2 Bradley; M113                                             | Manobra e apoio à infantaria mecanizada.                                |
| Artilharia               | M109; M270 MLRS (futuro)                                     | Apoio indireto, contra-bateria e concentração de fogo.                  |
| Defesa AA de canhão      | M163 Vulcan                                                  | Proteção próxima contra ameaças aéreas.                                 |
| Defesa AA curta          | M1097 Avenger; equipes Stinger                               | Cobertura móvel de curto alcance e MANPADS.                             |
| Defesa AA média/longa    | MIM-23 Hawk; Patriot (por cenário)                           | Defesa de instalações e zonas; radar e logística próprios.              |
| Radar / reconhecimento   | Postos de vigilância, radares de defesa aérea e observadores | Detecção, identificação e geração de contatos para a rede.              |
| Logística                | M977 HEMTT; caminhões de transporte; depósitos e oficinas    | Abastecimento, reparação, transporte e reabastecimento.                 |
| Engenharia e apoio       | Unidades de engenharia; recuperação de blindados             | Reparo de vias, pontes e resgate de equipamentos, fase posterior.       |

Nota de coerência histórica: o M1097 Avenger, o AH-64A e o M1A1 são escolhas apropriadas para um cenário dos anos 1990. Mísseis, variantes de radar e configurações específicas ficam vinculados à data escolhida.

## 10. Catálogo inicial EAST

| **Categoria**            | **Equipamentos candidatos**                                 | **Papel no sistema**                                                 |
|--------------------------|-------------------------------------------------------------|----------------------------------------------------------------------|
| Helicópteros de ataque   | Mi-24V/P Hind; Mi-28 ou Ka-50 (apenas cenários posteriores) | Ataque, apoio e capacidade limitada de transporte conforme variante. |
| Helicópteros utilitários | Mi-8 / Mi-17                                                | Transporte e apoio logístico.                                        |
| Carros de combate        | T-72B; T-80U (opcional)                                     | Força blindada principal e reservas mecanizadas.                     |
| Blindados de infantaria  | BMP-2; BTR-80; MT-LB                                        | Mobilidade da infantaria e apoio.                                    |
| Artilharia               | 2S1 Gvozdika; 2S3 Akatsiya; BM-21 Grad                      | Fogo indireto e concentração de artilharia.                          |
| Defesa AA de canhão      | ZSU-23-4 Shilka; ZU-23-2                                    | Cobertura próxima de unidades e instalações.                         |
| Defesa AA curta          | 9K35 Strela-10; 9K330 Tor; equipes Igla                     | Proteção de colunas, posições e forças móveis.                       |
| Defesa AA média/longa    | Buk; S-300 (por cenário)                                    | Proteção regional e de infraestrutura estratégica.                   |
| Radar / reconhecimento   | Radares de vigilância e observadores avançados              | Rede de detecção e compartilhamento de contatos.                     |
| Logística                | Ural-4320; depósitos; oficinas e veículos de recuperação    | Transporte de cargas, manutenção e recuperação.                      |
| Engenharia e apoio       | Pontes móveis; unidades de engenharia (futuro)              | Restabelecimento de mobilidade e infraestrutura.                     |

Mi-28 e Ka-50 não integram a seleção básica para 1990–1995: seriam conteúdo opcional de cenários posteriores/reforços, dependendo da variante e da cronologia. A lista não implica presença simultânea de todos os sistemas.

## 11. Organização das forças e defesa antiaérea

### 11.1. Estrutura de forças proposta

Cada facção organiza seus recursos em escalões abstratos: comando de teatro → setores/agrupamentos → batalhões ou unidades equivalentes → destacamentos e veículos individuais, quando relevante. Helicópteros são rastreados individualmente para manutenção, tripulação e missões. Forças terrestres distantes podem ser agregadas por pelotão/companhia para poupar processamento.

| **Componente**    | **WEST**                                      | **EAST**                                      |
|-------------------|-----------------------------------------------|-----------------------------------------------|
| Aviação de ataque | Esquadrão AH-64A; apoio UH-60                 | Esquadrão Mi-24V/P; apoio Mi-8                |
| Força terrestre   | Companhias M1A1 + M2/M113                     | Companhias T-72B + BMP-2/BTR-80               |
| Artilharia        | Baterias M109                                 | Baterias 2S1/2S3; BM-21 em escalão superior   |
| Defesa de ponto   | M163 / Avenger / Stinger                      | Shilka / Strela-10 / Igla                     |
| Defesa de área    | Hawk ou Patriot por cenário                   | Buk ou S-300 por cenário                      |
| Apoio             | Caminhões, depósitos, manutenção, engenheiros | Caminhões, depósitos, manutenção, engenheiros |

### 11.2. Modelo funcional de defesa aérea

Distinguir canhões AA, MANPADS, SAM de curto alcance e SAM de médio/longo alcance. Cada sistema possui posição, munição, mobilidade, condições operacionais, sensores disponíveis e cobertura limitada por terreno e observação. Radares e observadores alimentam uma rede de informação que pode sofrer atrasos, falhas e perdas de comunicação.

Sistemas de longo alcance são predominantemente elementos estratégicos/operacionais no protótipo; MANPADS e canhões antiaéreos oferecem ameaças locais ao helicóptero. Não pressupor detecção perfeita nem alcance eficaz absoluto.

### 11.3. Seleção mínima para a primeira versão

| **Papel**              | **WEST**       | **EAST**        |
|------------------------|----------------|-----------------|
| Helicóptero jogável    | AH-64A Apache  | Mi-24V/P Hind   |
| Blindado de IA         | M1A1 Abrams    | T-72B           |
| Logística              | M977 HEMTT     | Ural-4320       |
| Defesa AA próxima      | M163 Vulcan    | ZSU-23-4 Shilka |
| Infantaria AA abstrata | Equipe Stinger | Equipe Igla     |

Essa seleção mínima define tipos iniciais, não a quantidade de exemplares. SAMs guiados, radares avançados, artilleria e outros veículos são incorporados por incrementos após o funcionamento comprovado do núcleo.

## 12. Direção visual e tecnologia

The current engine uses C++20 and the optional 2D viewer uses raylib. The
technology choices below concern the future tactical application.

Visual old-school, poligonal/low-poly, inspirado em Gunship 2000 e Hind (1996), com leitura visual moderna: resolução nativa, silhuetas reconhecíveis, texturas simples, terreno por heightmap, neblina/LOD, poucos efeitos e HUD legível. Evitar simulação de cockpit nesta fase. Tanques jogáveis ficam para etapa posterior.

Tecnologia ainda não fechada: C++ para núcleo independente da apresentação; raylib é candidata preferencial para prototipagem 3D leve; Godot 4 com renderizador Compatibility permanece alternativa a ser comparada por protótipo e medições. O núcleo deve rodar sem renderizador, com relógio próprio, estados reproduzíveis e semente aleatória fixa.

## 13. Historical implementation plan and acceptance criteria

The table preserves the original milestone plan for the helicopter concept.
The README tracks the current engineering roadmap.

| **Marco**                   | **Entrega verificável**                                                                                             |
|-----------------------------|---------------------------------------------------------------------------------------------------------------------|
| M0 — Simulação textual      | Duas facções, mapa abstrato, unidades e instalações; tempo acelerado e log determinístico.                          |
| M1 — Recursos e manutenção  | Produção, depósitos, rotas, comboios, consumo e disponibilidade individual de aeronaves.                            |
| M2 — Comando e inteligência | IA com ordens/objetivos, reconhecimento, contatos desatualizados e decisões sob informação incompleta.              |
| M3 — Combate e moral        | Resolução abstrata, danos persistentes, retirada, fadiga, perdas e reforços externos.                               |
| M4 — Mapa 2D                | Visualização interativa e criação/alteração de missões com checagem de recursos.                                    |
| M5 — Voo 3D                 | Terreno low-poly, Apache/Hind controláveis, HUD, armas, simulação de inimigos próximos e sincronização dos estados. |

Critérios iniciais: (a) nenhum recurso aparece do nada; (b) missões consomem meios e tempo; (c) veículos perdidos permanecem perdidos; (d) aeronaves entram em fila de manutenção; (e) facções não recebem informação onisciente; (f) ordens e intervenção do jogador persistem; (g) combate próximo e distante produzem estados compatíveis; (h) campanhas podem ser repetidas sem gráficos para depuração.

## 14. Decisões pendentes — não presumidas

- Ano e teatro exatos: 1990–1995 é referência proposta, não data fechada.

- Escala do mapa, duração típica de campanha e ritmo do tempo simulado.

- Autoridade do jogador: piloto, comandante de esquadrão, comando operacional ou escolha de funções.

- Regras de reforços externos: periodicidade, gatilhos, disponibilidade e origem dos inventários.

- Detalhamento da economia: classes de munição, peças, combustível e dependências industriais.

- Tamanho inicial de cada facção e composição efetiva das unidades; catálogo de equipamento não é inventário.

- Métricas e algoritmo de combate abstrato, detecção, moral e avaliação de objetivos.

- Biblioteca gráfica definitiva após testes de desempenho no hardware-alvo.

## 15. Princípios de engenharia

Começar pelo modelo de dados e testes de campanha, não pelo voo. Priorizar implementações simples, mensuráveis e reproduzíveis. Manter conteúdo (veículos, instalações, missões e facções) orientado por dados para ampliação futura. Evitar vínculos diretos entre lógica estratégica e API gráfica. Adotar registro estruturado de eventos para explicar decisões, perdas, atrasos e mudanças de estado.

Documento consolidado a partir das decisões e propostas discutidas. Nomes comerciais e designações militares são referências de ambientação e classificação de unidades, sem pretensão de reprodução técnica integral.
