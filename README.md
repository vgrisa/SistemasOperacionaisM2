# M2 — Simulador de escalonamento multiprocessado com paginação sob demanda

Escalonadores FCFS, SJF (não preemptivo) e RR. Simula 1 ou 4 CPUs com uma fila de prontos global e
memória física paginada com substituição FIFO ou LRU. O simulador é em **C**, e o JSON é lido com a
biblioteca [cJSON](https://github.com/DaveGamble/cJSON) (MIT), que fica em `lib/`. Para compilar basta
o `gcc`, sem instalar nada. Os gráficos são gerados em Python.

| Arquivo | O que é |
| --- | --- |
| `simulador.c` | o simulador (argumentos, menu, modo ao vivo, resumo, Gantt, log) |
| `lib/cJSON.c`, `lib/cJSON.h` | biblioteca externa de JSON (v1.7.18), licença em `lib/cJSON_LICENSE` |
| `Makefile` | `make` compila; `make teste` roda os testes |
| `testes.py` | confere o `./simulador` contra os exemplos 5.4, 5.5, 7.1, 7.2 e 7.3 do enunciado |
| `experimentos.py` | roda o `./simulador` nas 52 execuções da seção 11 e gera tabelas, gráficos e Gantt |
| `cenarios/` | `cenario_A_t0.json` e `cenario_B_espacadas.json` (carga do grupo) |
| `exemplos/` | os exemplos pequenos do enunciado em JSON |
| `resultados/` | saída do `experimentos.py`: `tabelas.md`, `execucoes.csv`, gráficos `g*.png`, `gantt/` |
| [`RELATORIO.md`](RELATORIO.md) | desenho experimental, resultados e análise crítica |

## Como rodar (Codespaces ou qualquer Linux)

```bash
cd m2
make                                   # compila ./simulador
make teste                             # valida contra os exemplos do enunciado
```

```bash
./simulador --arquivo cenarios/cenario_B_espacadas.json --cpus 4 --sched rr --quantum 10 --subst lru --quadros 256
```

Todos os parâmetros vêm da linha de comando, sem precisar editar nem recompilar nada:

| Argumento | Valores | Padrão |
| --- | --- | --- |
| `--arquivo` | caminho do JSON | obrigatório |
| `--cpus` | 1 ou 4 | 1 |
| `--sched` | fcfs, sjf, rr | fcfs |
| `--quantum` | inteiro (só no RR) | 10 |
| `--subst` | fifo, lru | lru |
| `--quadros` | inteiro | 256 |
| `--custo-troca` | inteiro | 1 |
| `--custo-fault` | inteiro | 20 |
| `--log` | gera o log de estados (seção 9.3) | desligado |
| `--log-ate` | último instante gravado no log (0 = tudo) | 2000 |
| `--ao-vivo` | mostra a simulação acontecendo | desligado |
| `--atraso` | ms por unidade de tempo no modo ao vivo | 150 |
| `--saida` | pasta dos arquivos gerados | `saida/` |

Cada execução imprime o **resumo padronizado** (parâmetros, métricas globais, tabela por CPU e page
faults) e grava em `saida/`:

- `<execucao>_resumo.txt`: o mesmo resumo;
- `<execucao>_gantt.csv`: Gantt de 0 a 200, no formato `cpu,inicio,fim,evento`;
- `<execucao>_processos.csv`: espera, resposta e turnaround de cada processo;
- `<execucao>_metricas.txt`: as métricas em `chave=valor`, que o `experimentos.py` lê;
- `<execucao>_log.txt`: com `--log`, a cada instante em que algo muda grava os eventos, a fila de
  prontos em ordem, os bloqueados com o instante de retorno, o que está em cada CPU e os quadros
  ocupados. O log da simulação inteira passa de 200 MB, por isso o padrão vai até t=2000.

### Execução em tempo real

```bash
./simulador --arquivo cenarios/cenario_B_espacadas.json --cpus 4 --sched rr --quantum 10 --ao-vivo
```

A tela é redesenhada a cada unidade de tempo e mostra o estado de cada CPU (pid, TROCA ou OCIOSA), a
fila de prontos, os bloqueados com o instante de retorno, os quadros ocupados e os eventos daquele
instante. O `--atraso` controla a velocidade. `Ctrl+C` sai do modo ao vivo, a simulação continua em
velocidade máxima até o fim e o resumo é impresso.

### Menu interativo

Sem argumentos (`./simulador`), o programa pergunta cenário, CPUs, escalonador, quantum,
substituição, quadros, custos, log e modo ao vivo. Cada pergunta já vem com o valor padrão.

### Experimentos e gráficos

```bash
pip install matplotlib
make && python3 experimentos.py        # ~25 s
```

## Estruturas de dados

| Parte | Estrutura | Por quê |
| --- | --- | --- |
| Fila de prontos FCFS/RR | vetor circular | entra no fim e sai do início em O(1); cada processo está na fila no máximo uma vez, então cabe em N posições |
| Fila de prontos SJF | heap binário por `(restante do burst, ordem de entrada)` | menor burst em O(log n); empate = quem entrou primeiro |
| Bloqueados (E/S e fault) | heap binário por `(instante de retorno, pid)` | não formam fila: cada um tem seu tempo; no instante t só se retiram os que vencem em t |
| Quadros | vetor de quadros ligados numa lista dupla, na ordem de substituição | a cabeça é a próxima vítima. No FIFO a página entra no fim e o acerto não mexe; no LRU todo acerto move a página para o fim. A vítima é o primeiro quadro não protegido a partir da cabeça. Carga, remoção e "mover para o fim" são O(1) |
| Tabela de páginas | `quadro[pag]` em cada processo (−1 = fora da memória) | acerto ou fault em O(1); no término percorre só as páginas do processo para liberar os quadros |

O tempo avança de 1 em 1 unidade. Cada execução do cenário completo leva menos de 1 s.

## Ordem de eventos em cada instante t

1. Cada CPU encerra a unidade `[t-1, t)`: pode ser fim de burst (vai para E/S ou termina e libera os
   quadros) ou fim de quantum.
2. Entram na fila as chegadas e os retornos de E/S e de page fault, em ordem de pid.
3. O processo cujo quantum acabou vai para o fim da fila se há outro pronto. Se não há, continua sem
   custo e com novo quantum.
4. CPUs livres despacham, da menor para a maior. Se o processo é diferente do último daquela CPU, ela
   gasta `--custo-troca` em TROCA. Em seguida vem a referência da unidade: com acerto, executa
   `[t, t+1)`; com fault, o processo bloqueia por `--custo-fault`, a página fica protegida e a CPU
   tenta despachar de novo no mesmo instante.
