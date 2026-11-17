# Anexo 02 — Economia, produção e infraestrutura

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

## 1. Objetivo e limites

Representar estoques finitos, cadeias produtivas locais, instalações e seu
impacto operacional. Economia **não** desloca cargas (LOGISTICS), **não**
repara veículos (MAINTENANCE) e **não** decide prioridades militares
(OPERATIONS). Nenhum equipamento moderno surge automaticamente de uma fábrica.
Aeronaves e blindados completos entram por inventário inicial, reservas ou
reforços externos conforme `MILITARY_FORCES.md`.

## 2. Recursos e unidades

| Recurso | Unidade interna proposta | Produção ou origem |
|---|---|---|
| CrudeOil | litro inteiro | extração / reserva inicial |
| Fuel | litro inteiro | refinaria / importação |
| Ammunition[class] | unidade inteira | estoque / entregas externas; fabricação local opcional futura |
| SpareParts[class] | peça equivalente inteira | fábrica / importação |
| IndustrialMaterials | unidade equivalente inteira | fábrica / importação |
| GeneralSupplies | unidade equivalente inteira | fábrica / importação |
| Personnel | pessoas | lotação / reforços, nunca fabricadas |
| VehicleInventory[type] | unidades físicas com ID | reservas / reforços |

Evitar números de ponto flutuante para transferências e estoques: usar inteiros
não negativos e taxas fracionárias acumuladas com resto determinístico. Classes
são identificadores de dados de cenário, não enum com lista fechada. Cada
recurso tem unidade documentada; converter litros para quantidades consumidas
pela aeronave somente em ponto explícito do sistema.

## 3. Entidades mínimas

- `Stockpile`: id, owner_faction, location_id, capacity_by_resource,
  quantity_by_resource, reserved_by_resource, status.
- `Installation`: id, type, sector_id, operator_faction, integrity,
  operational_state, workers_available, power_available, input_stockpile_id,
  output_stockpile_id, repair_work_remaining.
- `ProductionRecipe`: installation_type, inputs, outputs, work_per_batch,
  power_required, workforce_required; versão do catálogo.
- `ProductionJob`: id, installation_id, recipe_id, progress,
  last_processed_tick.
- `EconomyLedgerEntry`: tick, actor, source, destination, resource, amount,
  reason, event_sequence. Estoque local pertence a **um único** depósito.

Separar titularidade/operador de instalação do controle de setor; a mudança
operacional depende de evento de captura de `TERRITORY.md`. Instalação capturada
conserva danos e estoques físicos até resolução explícita de saque/perda;
nunca transfere bens já em transporte.

## 4. Invariantes de conservação

1. `0 <= reserved <= quantity <= capacity` em cada estoque.
2. Transferências locais são atômicas: débito + crédito ou nenhuma alteração.
3. Produção só credita resultado após debitar insumos e avançar trabalho.
4. Falta de energia, trabalhadores, insumos ou capacidade de saída interrompe
   produção, sem progresso oculto.
5. Destruição de estoque registra perda explícita; perdas não evaporam sem
   evento e quantidade auditável.
6. Entradas externas têm evento `EXTERNAL_DELIVERY`, nunca criação implícita.
7. Consumo de missão, reparo e preparação é debitado uma única vez pelo
   responsável, por requisição idempotente identificada.

## 5. Produção por intervalo

A cada tick econômico: ordenar instalações por ID; verificar operador,
funcionamento, energia, mão de obra, receita e espaço de saída. Aplicar
capacidade como fator de produtividade `integrity_factor` configurável.
Acumular trabalho em unidades inteiras; completar lotes somente quando os
insumos da receita puderem ser reservados/debitados integralmente. Recomenda-se
reservar insumos ao início do lote e creditar produtos ao fim; cancelar lote
restitui apenas insumos não consumidos conforme política documentada. Para v0,
preferir lote atômico ao completar, sem reserva antecipada, e não avançar
quando faltarem insumos.

Fluxo de referência: campo produz petróleo -> transporte -> refinaria consome
petróleo + energia -> produz combustível -> transporte -> base. Não criar
produção infinita de petróleo: campo tem reserva física inicial finita e taxa
de extração; campos opcionais com reposição devem declarar explicitamente a
fonte do recurso.

## 6. Danos, captura e reparação

Integridade é `0..100` por default, sem arredondamentos implícitos. Abaixo de
limiar configurável, instalação não opera; danos reduziriam a capacidade em
outras faixas. Após captura, operador muda pelo evento territorial, mas
reativação depende de estabilização, energia, suprimentos e reparos. Reparo de
**instalação** consome materiais e trabalho locais; reparo de **aeronave**
pertence a MAINTENANCE. Somente um módulo altera cada estado autoritativo.

## 7. Interfaces e ordem

- Entrada: eventos de captura/dano, oferta de pessoal e energia, entregas
  concluídas, requisições aceitas de consumo e reparo.
- Saída: disponibilidade por estoque, capacidade de produção, requisição de
  transporte (não um comboio), eventos `PRODUCTION_COMPLETED`,
  `PRODUCTION_BLOCKED`, `STOCK_CHANGED`, `INSTALLATION_DISABLED`.
- Uma fase de produção pode gerar disponibilidade para logística **no tick
  seguinte**, evitando dependência da ordem entre entregas e produção.
- Reserva orçamentária não equivale a entrega: reservar diminui quantidade
  livre, mas mantém estoque físico até `LOGISTICS` efetivar coleta.

## 8. Configuração de cenário (exemplo, não schema definitivo)

```yaml
economy:
  update_interval_minutes: 5
  industrial_integrity_min_percent: 25
  refinery:
    batch_inputs: {CrudeOil: 1000}
    batch_outputs: {Fuel: 800}
    work_minutes: 60
    requires_power: true
```

Números não são dados históricos. Não fixar catálogo de receitas no código.

## 9. Testes mínimos

- E01 estoque insuficiente impede débito e produção; não fica negativo.
- E02 capacidade de saída insuficiente bloqueia lote, sem perda de insumos.
- E03 refinaria sem energia não produz; retoma quando energia retorna.
- E04 perda de 30 unidades em ataque gera evento e balanço correspondente.
- E05 captura conserva integridade e estoques; reativação não é instantânea.
- E06 duas demandas simultâneas são atendidas por ordenação estável.
- E07 seed e comandos iguais geram mesmo ledger/hash.
- E08 salvar/carregar lote parcial preserva resultado final.
- E09 perda de campo petrolífero reduz produção futura, sem apagar Fuel já
  estocado em bases ou comboios.

## 10. Marcos sugeridos

E0: estoques, débitos e ledger; E1: receitas e instalações; E2: danos/captura;
E3: integração com transporte, manutenção e relatórios. Cada marco headless.

## 11. A definir antes do balanceamento

Tipos exatos de munição, equivalências de peças, fontes de energia,
produtividade, distribuição inicial de depósitos, tratamento de mão de obra
civil, limites de reserva e regras de fabricação local de armamento simples.
