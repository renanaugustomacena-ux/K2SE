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
python tools/verify_offsets.py          # ogni indirizzo, sul tuo eseguibile
python tools/extract_routine_table.py   # ricostruisce le 877 routine
python tools/q1_q2_compiler_test.py     # dimostra che il compilatore accetta ID nuovi
python tools/routine_id_test.py          # tabella C++ / header nss / bytecode emesso
```

---

## Cosa funziona già oggi

Due strumenti girano **subito**, senza compilare niente, e producono risultati verificabili sul tuo eseguibile.

### `tools/verify_offsets.py` — verifica tutti gli indirizzi

```
python tools/verify_offsets.py
```

Rilegge dal binario **ogni** indirizzo su cui K2SE si appoggia: le sonde derivate a mano, i cinque import GL risolti *per nome* dalla import directory, tutte le 102 righe della tabella, e la freschezza dell'header generato. Output atteso: `ALL PROBES PASSED`.

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

## Le funzioni disponibili oggi

Dodici routine estese, dall'ID 877 in su. **Quattro sono provate in gioco**
(877–879 e la sentinella `abs()`); le altre otto sono verificate staticamente ma
**aspettano ancora una sessione reale** — è scritto qui perché la differenza
conta.

| ID | Funzione | Stato |
|---|---|---|
| 877 | `K2SE_GetVersion()` | ✅ in gioco |
| 878/879 | `K2SE_SelfTest`, `K2SE_ReportTest` | ✅ in gioco |
| 880 | `K2SE_EchoString` — round trip delle stringhe | ⏳ da provare |
| 881 | `K2SE_GetAbilityScoreBase(oCreature, nAbility)` | ⏳ da provare |
| 882 | `K2SE_GetSkillRankBase(oCreature, nSkill)` | ⏳ da provare |
| 883 | `K2SE_GetFeatAcquired(oCreature, nFeat)` | ⏳ da provare |
| 884 | `K2SE_GetSpellAcquired(oCreature, nSpell)` | ⏳ da provare |
| 885–888 | controllo nebbia a runtime | ⏳ da provare, **opt-in** |

881–884 sono **sole letture**, di proposito: camminano su offset di struttura
importati, e un offset sbagliato in lettura costa un numero sbagliato mentre in
scrittura corromperebbe un salvataggio. I mutatori arrivano quando le letture
sono confermate.

La nebbia si accende solo se esiste un file `k2se_fog.txt` accanto
all'eseguibile: è l'unica parte di K2SE che gira sul thread di rendering, e non
ha il diritto di destabilizzare una DLL che funziona.

## La tabella degli indirizzi

`data/k2se_addresses.csv` — 102 indirizzi, ognuno con **provenienza** e **come è
stato verificato su questo binario**. `src/offsets_generated.h` ne è generato;
`src/offsets.h` conserva le costanti derivate a mano e le confronta con quelle
importate tramite `static_assert`, così due derivazioni indipendenti che
divergono diventano un errore di compilazione.

Metà della tabella viene dal database di
[Kotor-Patch-Manager](https://github.com/LaneDibello/Kotor-Patch-Manager).
**Solo dati**: nessuna riga di codice altrui è stata copiata. Ogni indirizzo
importato è stato ricontrollato contro l'eseguibile prima di essere ammesso —
48 su 48 stanno in `.text` dietro un prologo MSVC — e tutti e 10 gli indirizzi
che K2SE aveva già derivato a mano coincidono con i loro.

```
python tools/import_kpm_db.py       # importa + verifica
python tools/gen_offsets.py         # CSV -> header
python tools/routine_id_test.py     # tabella C++ / header nss / compilatore
python tools/export_to_kpm.py       # contributo verso monte
```

## Cosa manca (in ordine)

*(Q1–Q7 e Q11 sono chiuse. Dettaglio in [DESIGN.md §7](DESIGN.md).)*

**Una sessione di gioco, ed è il passo che blocca tutto il resto.** Otto delle
dodici routine estese non hanno mai girato. La verifica statica — prologhi,
`ret imm16`, byte ACTION emessi, build pulita — è necessaria ma **non
sufficiente**, e questo progetto non tratta l'una per l'altra.

| Da provare | Perché è il rischio maggiore |
|---|---|
| **880** stringhe | La stringa che torna deve essere identica. Se differisce, l'ABI è sbagliata — non c'è altro che possa essere |
| **881–884** creature | Camminano su **offset di struttura**: affermazioni sul layout a runtime, importate, non controllabili leggendo il file |
| **885–888** nebbia | Thread di rendering, e riscrive i fragment program in volo |

Poi, in ordine: sbloccare i mutatori se le letture sono giuste, chiudere i tag
degli engine structure, contattare LaneDibello.

---

## Com'è fatto

```
src/offsets.h      indirizzi derivati a mano, con la provenienza di ognuno
src/offsets_generated.h  generato da data/k2se_addresses.csv -- non modificare
src/vmstack.*      i 12 accessor dello stack della VM
src/exostring.*    CExoString, guidando i costruttori del motore
src/gameobj.*      OBJECT id -> puntatore vivo, difensivo a ogni salto
src/glhook.*       nebbia a runtime (opt-in)
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
