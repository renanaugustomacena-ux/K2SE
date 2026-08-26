# K2SE — KOTOR 2 Script Extender

Estende NWScript in **Star Wars: Knights of the Old Republic II** (build Aspyr/Steam 1.0.2.0) con nuove funzioni engine chiamabili da `.nss`, tramite una DLL proxy che si aggancia alla VM del gioco.

Stato: **funzionante in gioco.** Una routine NWScript che non esisteva nel motore risponde a script compilati, dentro una partita reale.

Design completo, con la provenienza di ogni indirizzo: **[DESIGN.md](DESIGN.md)**.

---

## La prova

Estratto reale da `%LOCALAPPDATA%\K2SE\k2se.log`, sessione del 26/08/2026 su
`swkotor2.exe` 1.0.2.0 (Steam/Aspyr, con patch 4GB applicata):

```
fingerprint OK
extended routines registered: 1 (first free vanilla ID is 877)
  id 877 -> K2SE_GetVersion
hook installed: [0x009940D8] 0x00668FD0 -> ... (old protection 0x2)
K2SE_LOAD_OK
CALLER of ExecuteCommand: return address 0x0070029B  (first dispatch: id=548, nParams=0)
self-validation, table @0x261B4168:
  cmds[  0] = 0x0068F5D0  expected 0x0068F5D0  Random                       OK
  cmds[ 71] = 0x0068C4A0  expected 0x0068C4A0  acos (shared math handler)   OK
  cmds[ 77] = 0x0068C4A0  expected 0x0068C4A0  abs (shared math handler)    OK
  cmds[876] = 0x0069C460  expected 0x0069C460  RebuildPartyTable            OK
  NULL slots: 0 (expected 0)
self-validation PASSED
*** FIRST EXTENDED ROUTINE CALL: id=877 (K2SE_GetVersion), nParams=0 ***
  [trace] extended routine 877 (K2SE_GetVersion), nParams=0
  [trace]   -> pushed 100, rc=0
```

Nella stessa sessione: **1294 chiamate alla routine 877, 1294 push riusciti,
zero errori**, gioco stabile per oltre due ore di CPU senza crash né freeze.

`self-validation PASSED` è la riga che conta più di tutte: la tabella delle 877
routine ricostruita **leggendo il file su disco** coincide con quella che il
gioco costruisce **sull'heap mentre gira**.

### Verificalo tu

Non serve fidarsi di niente di quanto sopra. Clona ed esegui:

```
python tools/verify_offsets.py          # 11 probe sul tuo eseguibile
python tools/extract_routine_table.py   # ricostruisce le 877 routine
python tools/q1_q2_compiler_test.py     # dimostra che il compilatore accetta ID nuovi
```

---

## Cosa funziona già oggi

Due strumenti girano **subito**, senza compilare niente, e producono risultati verificabili sul tuo eseguibile.

### `tools/verify_offsets.py` — verifica tutti gli indirizzi

```
python tools/verify_offsets.py
```

Rilegge dal binario ogni indirizzo su cui K2SE si appoggia. Output atteso: `ALL PROBES PASSED`.

```
OK    RTTI class name @0x00A0F4F8 -> .?AVCSWVirtualMachineCommands@@
OK    vtable[2] ExecuteCommand   @0x009940D8 = 0x00668FD0
OK    alloc size 877*4           @0x00665F5A = 0x00000DB4
OK    dispatch bound 877         @0x00668FDC = 0x0000036D
...
```

### `tools/extract_routine_table.py` — ricostruisce la tabella delle routine

```
python tools/extract_routine_table.py
```

La tabella delle 877 routine è **allocata sull'heap a runtime**: non esiste da nessuna parte nel file. Ma il codice che la riempie sì. Questo tool decodifica gli store letterali di `InitializeCommands` e dell'installer dei minigiochi, e ricostruisce l'intera mappa `ID → handler` senza debugger.

```
InitializeCommands  @0x00665F50 : 776 stores
minigame installer  @0x006F5B80 : 103 stores
distinct slots filled : 877 / 877
handlers NOT starting with 55 8B EC : 0
IDs 67..77 (fabs..abs) distinct handlers: 1 ['0x0068C4A0']
TABLE FULLY RECOVERED
```

Quell'ultima riga è la prova strutturale decisiva: gli 11 ID da 67 a 77 (`fabs, cos, sin, tan, acos, asin, atan, log, pow, sqrt, abs`) condividono **un solo handler**, e i nomi coincidono con la lista indipendente di xoreos-tools. Non è una coincidenza: è la tabella giusta.

Produce `tools/k2_routine_table.csv`, importabile in Ghidra per etichettare tutti e 655 gli handler.

---

## Compilare la DLL

**Servono:** Visual Studio 2022 (workload *Sviluppo desktop con C++*, **toolset x86**), CMake ≥ 3.21, Python 3.

```bat
python build\generate_proxy.py
cmake -S . -B build -A Win32
cmake --build build --config Release
```

Il risultato è `build\Release\version.dll`: 32-bit, CRT statico, importa solo KERNEL32.

`generate_proxy.py` legge la tabella degli export del **vero** `C:\Windows\SysWOW64\version.dll` invece di usare una lista scritta a mano, così il proxy combacia sempre col sistema ospite. Inoltra **tutti e 17** gli export, non solo i 3 che serve al gioco: qualcos'altro nel processo (antivirus, overlay, RivaTuner) potrebbe importare `version.dll`, e un export mancante lo romperebbe.

## Installare

Copia `version.dll` accanto a `swkotor2.exe`. Basta. Per disinstallare, cancellala.

Il log finisce in `%LOCALAPPDATA%\K2SE\k2se.log` — **mai** nella cartella del gioco.

> **Perché lo slot `version.dll`** e non `opengl32.dll`: quest'ultimo è conteso da Mesa, ReShade, ShaderOverride e dal fix nebbia della community — collisione garantita. E nemmeno `binkw32.dll`, che è un file del depot Steam e viene ripristinato da "Verifica integrità". `version.dll` non è importato da nient'altro nel processo ed essendo un file *aggiunto* sopravvive alla verifica.

## Come lo usa un modder

```c
#include "k2se"

void main()
{
    if (!K2SE_IsPresent()) return;   // degrada in silenzio, mai un crash
    // ... chiamate K2SE ...
}
```

`K2SE_Version()` funziona con l'**`nwscript.nss` di serie**: viaggia sopra `abs()` (routine vanilla 77) con un argomento magico. Costa zero ID nuovi e su una macchina senza K2SE restituisce semplicemente 0.

Questo risolve un problema reale: uno script **non può** testare la presenza dell'extender chiamando una funzione dell'extender — quella chiamata *sarebbe* il guasto.

---

## Cosa manca (in ordine)

Tre esperimenti da fare **prima** di scrivere altro codice — meno di un'ora in tutto. Dettaglio in [DESIGN.md §7](DESIGN.md).

| | Domanda | Come | Blocca |
|---|---|---|---|
| **Q1** | `nwnnsscomp` accetta un `nwscript.nss` oltre 876 ed emette `05 00 03 6D <argc>`? | Appendi un prototipo, compila, hex dump del `.ncs` | tutte le routine nuove |
| **Q2** | Il compilatore salta in silenzio la routine 767 malformata, sfalsando tutto da 768 in su? | Compila `GetItemComponent()`: l'ACTION deve essere `05 00 03 03 00` | header esteso |
| **Q3** | **Cosa fa la VM con `-2002`? C'è un secondo bounds check a monte del dispatcher?** | `.ncs` fatto a mano con solo `05 00 03 6D 00`, breakpoint a `0x00668FE0`, **step out** | l'intera strategia "appendi ID" |
| **Q5** | Contratto esatto di `StackPopInteger`/`StackPushInteger` | Breakpoint dentro un handler vanilla, ispeziona ECX/`[esp+4]`/EAX | *tutto* ciò che tocca argomenti |

**Q5 è il motivo per cui `K2SE_ENABLE_STACK_ABI` in `src/routines.cpp` è a `0`.** Gli accessor dello stack a `0x006FD9A0` / `0x006FD9C0` sono l'unica parte del design **non verificata** contro il binario. Chiamarli a intuito corromperebbe lo stack della VM — e non crasherebbe in modo pulito: produrrebbe un gioco sottilmente sbagliato. Finché il flag è a 0 la DLL si aggancia e inoltra tutto, senza mai toccare un argomento.

---

## Com'è fatto

```
src/offsets.h      tutti gli indirizzi, con la provenienza di ognuno
src/fingerprint.*  verifica di essere sulla build giusta -> altrimenti NON installa niente
src/vm.*           scambio dello slot di vtable + thunk di dispatch + auto-validazione
src/routines.*     registro delle routine estese + sentinella abs()
src/log.*          logging Win32 grezzo (niente CRT: deadlock a DLL_PROCESS_DETACH)
src/dllmain.cpp    ordine di init
```

**Il punto di aggancio è un solo dword.** `ExecuteCommand` è lo slot [2] della vtable di `CSWVirtualMachineCommands` a `0x009940D8`: si sostituisce quel puntatore, salvando l'originale come trampolino. Niente MinHook, niente detour inline, niente trampolini generati — installazione atomica, disinstallazione banale.

`DllMain` gira **prima** dell'entry point dell'exe, quindi prima che `CSWVirtualMachineCommands` venga costruito: è questo che rende sufficiente lo scambio dello slot.

**Postura di sicurezza: al primo dubbio, non installare nulla.** Un hook presente ma inerte è peggio di nessun hook. Il fingerprint controlla la vtable, i due bound a 877, l'allocazione `0x0DB4` e gli offset di struttura — **mascherando il campo Characteristics**, perché la patch 4GB/LAA lo cambia da `0x0103` a `0x0123` e moltissimi utenti reali (incluso chi scrive) hanno l'exe patchato. Un hash dell'intero file rifiuterebbe proprio loro.

Alla prima chiamata reale, K2SE si auto-valida contro la tabella viva: `cmds[0]`, `cmds[71]`, `cmds[77]`, `cmds[876]`, più il conteggio degli slot NULL. Se qualcosa non torna, disinstalla l'hook e lascia girare il gioco vanilla.

---

## Prima di pubblicare

Contatta **LaneDibello** ([Kotor-Patch-Manager](https://github.com/LaneDibello/Kotor-Patch-Manager), MIT): ha già `AddressDatabases/kotor2_steam_aspyr.db` agganciato a questo identico binario e in roadmap 2026 ha "migliore supporto KOTOR 2". O K2SE diventa la metà-K2 di quell'ecosistema, o duplica un anno di lavoro in Ghidra.

Il criterio di successo non è "una funzione nuova gira", è **"almeno una mod pubblicata dipende da K2SE"**. Lo Script Extender per K1 di KPM funziona da un anno e ha zero mod dipendenti.

## Licenza

MIT — come entrambi i precedenti dell'ecosistema.
