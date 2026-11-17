# Anexo 05 — Forças WEST/EAST, catálogo e reforços

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

## 1. Objetivo e recorte temporal

Facções fictícias WEST e EAST, com equipamentos característicos do Ocidente
e do bloco soviético/russo. Referência tecnológica proposta: 1990–1995;
variantes e teatro exatos continuam **não definidos**. Nomes reais servem como
identificação de arquétipos; parâmetros do jogo são abstrações balanceáveis,
não tabelas de desempenho militar certificados. Não presumir superioridade
universal de um bloco.

## 2. Organização de forças

`Faction -> Formation -> Unit -> Vehicle/Team` e `Base -> Squadron -> Aircraft`.
Formação agrega unidades sob comandante, setor, ordens, coesão e inventário;
unidade possui ID persistente, facção, tipo, posição, prontidão, moral,
experiência, suprimentos, equipe e composição. Veículos são individuais quando
manutenção, perda ou munição particular importar; infantaria/manpads podem ser
agregados em equipes. `unit_id` não muda na retirada ou reforço; unidades
substituídas obtêm novos IDs. Definir `ability_to_occupy` explicitamente:
helicópteros não capturam setores, mesmo quando dominam o espaço aéreo.

## 3. Catálogo WEST

| Categoria | Tipo inicial / expansão | Função |
|---|---|---|
| Ataque aéreo | AH-64A Apache **inicial** | ataque/reconhecimento armado; jogável no futuro |
| Transporte aéreo | UH-60 Black Hawk | transporte, reabastecimento leve, evacuação eventual |
| MBT | M1A1 Abrams **inicial** | manobra terrestre, ocupação mediante componente terrestre |
| IFV | M2 Bradley | infantaria mecanizada/apoio |
| Artilharia | M109 | fogo indireto abstrato |
| Logística | M977 HEMTT **inicial** | transporte de suprimentos |
| AAA | M163 Vulcan **inicial** | defesa aérea de proximidade |
| SHORAD | M1097 Avenger | defesa móvel com mísseis |
| SAM longo alcance | Patriot | defesa de área (cenário, não primeira entrega) |
| MANPADS | Stinger **inicial abstrato** | ameaça portátil/defesa de baixa altitude |
| Sensores | radar de vigilância WEST genérico | observação e alerta, não onisciência |

## 4. Catálogo EAST

| Categoria | Tipo inicial / expansão | Função |
|---|---|---|
| Ataque aéreo | Mi-24V/P Hind **inicial** | ataque e transporte limitado; jogável no futuro |
| Transporte aéreo | Mi-8 | transporte e logística |
| MBT | T-72B **inicial** | manobra terrestre |
| IFV | BMP-2 | infantaria mecanizada/apoio |
| Artilharia | 2S1 Gvozdika | fogo indireto abstrato |
| Logística | Ural-4320 **inicial** | transporte de suprimentos |
| AAA | ZSU-23-4 Shilka **inicial** | defesa de baixa altitude |
| SHORAD | Strela-10 / Tor | defesa móvel curta (variantes por cenário) |
| SAM médio/longo | Buk / S-300 | defesa de área (cenário, não primeira entrega) |
| MANPADS | Igla **inicial abstrato** | ameaça portátil |
| Sensores | radar de vigilância EAST genérico | observação e alerta |

**Catálogo não é inventário.** Configuração inicial é um cenário separado com
quantidades explícitas. Modelos listados como expansão não implicam que tenham
simulação detalhada na primeira versão. Compatibilidade histórica de variantes
e configuração de armamentos devem ser revistas na definição do cenário.

## 5. Dados de equipamento orientados por dados

`EquipmentType`: id, category, faction_compatibility, roles, mass_or_size_class,
movement_class, fuel_type, fuel_capacity, consumption_rate, ammo_slots,
maintenance_class, spare_parts_class, crew_required, sensor_profile,
weapon_profile, vulnerability_profile, logistics_capacity,
can_occupy, optional_is_playable. Usar tipos explícitos e validação de
referências; um mesmo tipo não deve duplicar lógica de combate e manutenção.
Dados específicos devem declarar unidades, faixa válida e versão de schema.
Um perfil de capacidades abstratas é suficiente na primeira simulação.

## 6. Diferenças doutrinárias sem bônus ocultos

WEST pode iniciar cenários com mais preferência por reconhecimento, precisão
e conservação de aeronaves; EAST pode preferir concentração de formações,
artilharia e defesa aérea em camadas. Essas são **políticas configuráveis por
comandante**, não atributos raciais/nacionais nem bônus constantes. Estoques,
nível de treinamento, comando, experiência e inteligência definem o desempenho
concreto das unidades em cada campanha.

## 7. Reforços externos finitos

`ReservePool`: faction, type_id, available_count, source, eligibility.
`ReinforcementOrder`: id, faction, inventory_ids_or_count, trigger,
authorization_tick, eta_tick, entry_node_id, status.

Uma reserva deixa de estar disponível **ao autorizar** um reforço. A chegada
consome tempo, requer nó de entrada funcional/seguro e cria unidades reais
somente após evento `REINFORCEMENT_ARRIVED`. Se entrada for bloqueada,
reforço aguarda ou desvia por regra configurável, nunca surge no destino.
Reforços podem incluir tipos novos (p.ex. aeronaves externas) sem exigir
fabricação local; quantidades não são infinitas. Não permitir que o comando
requisite sem limite mais unidades que seu pool possui.

O encerramento de guerra (`TERRITORY.md`) deve avaliar reforços aprovados que
possam chegar dentro do horizonte configurado antes de declarar incapacidade
irreversível. Reforços já em trânsito constam no save.

## 8. Contratos

- OPERATIONS atribui ordens a unidades e solicita reforços elegíveis.
- MAINTENANCE controla disponibilidade individual e equipes.
- ECONOMY controla estoques de peças, munição, combustível e materiais.
- LOGISTICS controla transporte físico e local de entrada.
- INTELLIGENCE relata contatos sem expor catálogo inimigo não identificado.
- COMBAT aplica perdas e altera composição física, gerando eventos únicos.

Eventos: `UNIT_CREATED`, `UNIT_ASSIGNED`, `UNIT_LOST`, `RESERVE_ALLOCATED`,
`REINFORCEMENT_SCHEDULED`, `REINFORCEMENT_DELAYED`,
`REINFORCEMENT_ARRIVED`.

## 9. Testes

- F01 tipos WEST e EAST carregam do catálogo e IDs são únicos.
- F02 equipamento sem capacidade de ocupação não captura setor.
- F03 unidade destruída é removida da prontidão, sem reaparecer.
- F04 reservar reforço subtrai da reserva e impede duplicação.
- F05 reforço só entra após ETA e validada instalação de entrada.
- F06 perda de nó de entrada atrasa/desvia reforço sem teleporte.
- F07 novas classes podem ser adicionadas via dados sem alterar enum central.
- F08 mesma seed + cenário gera mesma sequência de reforços.
- F09 após save/load não há segunda entrega do mesmo reforço.

## 10. Etapas e questões abertas

F0 catálogo e IDs; F1 instâncias e formações; F2 pools/reforços; F3 perfis
operacionais mais detalhados. Pendências: tamanhos das formações, quantidade
inicial, inventários por base, taxas de reposição, sequência de novas armas,
precisão dos nomes/variantes de época e regras de reconstituição de unidades.
