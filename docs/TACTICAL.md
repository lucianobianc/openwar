# Anexo 09 — Voo arcade, ambiente 3D e integração tática

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

## 1. Objetivo e limitação de escopo

Integrar voo arcade de helicópteros ao estado real da campanha, sem fazer o
jogo recalcular toda a guerra em 3D. Esta etapa é **posterior** ao núcleo
estratégico, logística, combate abstrato e mapa 2D. O alvo é i5-1135G7,
Intel Iris Xe e 8 GB RAM; nenhuma meta de FPS é presumida sem medição.
Preferir cenário visual low-poly, inspirado em Gunship 2000 e Hind (1996),
resolução moderna e HUD funcional. Cockpits detalhados e tanques jogáveis
ficam fora do escopo inicial.

## 2. Tecnologia

C++20 para simulação; a biblioteca gráfica continua pendente de protótipo.
raylib é candidata preferencial, Godot Compatibility é alternativa a comparar,
mas `campaign/` não inclui cabeçalhos ou tipos da biblioteca escolhida.
As simulações estratégicas e operacionais devem rodar headless, sem janela.
Não introduzir física avançada ou engine própria completa sem evidência.

## 3. Autoridade sobre entidades

Cada entidade persistente possui um ID e um **único modo autoritativo**:
`ABSTRACT` (operacional) ou `TACTICAL` (cena ativa). A ativação usa snapshot
em fronteira de tick, inclui posição, rota, condição, munição, combustível,
ordens, tripulação e estado de missão. A transição é transação atômica:

1. Marcar entidade `PENDING_TACTICAL`, bloquear resolução abstrata futura.
2. Criar representação 3D com mesmo ID e estado inicial validado.
3. Confirmar `TACTICAL` e instante inicial; falha reverte a transação.
4. Simular física/arma localmente com relógio tático subordinado ao global.
5. Ao sair da área, consolidar perdas, posição e recursos; marcar `ABSTRACT`.

Jamais manter dois objetos autoritativos para a mesma entidade. Nunca
interpolar um dano abstrato e um dano tático sobre o mesmo intervalo.

## 4. Frequências e relógio

`SimulationClock` é fonte única. Estratégia utiliza passos discretos, por
exemplo 60 s simulados, e física usa subticks fixos, por exemplo 1/60 s.
Acumulador de tempo pode interpolar visualmente, mas não modifica tick global.
Pausar ou acelerar tempo altera agenda simulada de todos os sistemas de modo
consistente. Não misturar relógio de parede com duração de manutenção.

## 5. Voo e armas arcade

Modelo inicial: empuxo coletivo simplificado, inclinação/pitch/roll,
autorrotação fora do escopo; velocidade e subida limitadas por perfil de
helicóptero; controles legíveis, resposta inercial suave, colisão com terreno.
Dados de massa, estabilidade e armas são parâmetros de jogo, não reprodução
certificada de engenharia. Armas v0: canhão/mitralhadora e foguetes; mísseis
guiados e contramedidas entram após sensores e defesa aérea abstrata.
Disparo consome munição na entidade individual e gera evento de impacto/dano.
Não recriar estoque em cada troca de nível.

## 6. Terreno e gráficos

Terreno de heightmap, tiles com LOD, geometria econômica, edifícios e árvores
simplificados, distâncias de renderização configuráveis, neblina. Setor
estratégico `(x,y)` deve mapear precisamente para coordenadas em metros no
terreno 3D; adotar origem, eixo, escala e convenção explicitamente documentadas.
O desenho do domínio territorial não altera material físico do terreno,
apenas camada estratégica de mapa. Renderizar somente entidades próximas;
distantes continuam no simulador abstrato.

## 7. HUD mínimo

Altitude, velocidade, rumo, condição, combustível, armamento atual/munição,
waypoint, indicação de alvo conhecido e status da missão. Alvos desconhecidos
não recebem marcadores secretos. HUD consome `FactionView` e estado da própria
aeronave, nunca o estado verdadeiro oculto de inimigos. Interface opcional de
depuração fica separada.

## 8. Sensores e interação com a campanha

Sensores táticos produzem `Observation` para INTELLIGENCE (com fonte, tempo,
qualidade); tiro produz dano para COMBAT/estado autoritativo e eventos
operacionais; retorno produz horas de voo e trabalho para MAINTENANCE.
Missões criadas pelo jogador/IA usam waypoints estimados. Abater um comboio
prejudica o estoque em trânsito realmente existente. Destruir uma instalação
não reduz estoques de outras instalações magicamente.

## 9. Políticas de descarregamento da cena

Ao sair da bolha ativa ou encerrar voo, consolidar em fronteira segura de
subtick, encerrar projéteis pendentes por política explícita, persistir estado,
entregar autoridade ao ABSTRACT e registrar evento único. Se um míssil ainda
estiver em voo, manter efeito tático até resolver ou representar como evento
programado com ID, jamais perdê-lo ou duplicá-lo silenciosamente.

## 10. Testes e profiling

- T01 ativar/desativar entidade conserva ID, posição e recursos.
- T02 entidade em TACTICAL não recebe resultado de combate abstrato duplicado.
- T03 dano tático permanece no estado após saída e save/load.
- T04 disparos reduzem munição física uma só vez.
- T05 HUD não revela alvo desconhecido da facção.
- T06 trajetória perto da borda de setor preserva mapeamento metros/setor.
- T07 sair da área não teletransporta entidades ou cargas.
- T08 salvar/carregar em transição mantém invariantes.
- T09 simulação headless ainda compila e roda sem biblioteca gráfica.
- T10 perfil de FPS/memória é registrado no hardware-alvo com cenários fixos.

## 11. Marcos e decisões pendentes

T0 protótipo de terreno e voo isolado; T1 mapa de metros e entidades; T2 handoff
autoritativo e armas; T3 sensores/HUD e sincronização com a campanha.
A definir: biblioteca gráfica após benchmark, tamanho da bolha, modelo de
física arcade, terrain tile format, shaders, resolução e targets de desempenho.
