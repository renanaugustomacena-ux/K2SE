# K2SE — KOTOR 2 Script Extender · Documento di design

**Versione:** 1.0 — 26/08/2026
**Target:** `swkotor2.exe`, build Aspyr/Steam, FileVersion 1.0.2.0, TimeDateStamp `0x5603005D`, 6.588.416 byte
**Approccio approvato:** A — proxy DLL indipendente; primo traguardo = *walking skeleton* (proxy → hook della VM → una nuova funzione NWScript che gira davvero in gioco)
**API pubblica:** NWScript puro (nuove funzioni engine chiamabili da `.nss`)
**Prima feature reale dopo lo skeleton:** controllo runtime di nebbia/atmosfera

---

## 0. Sintesi esecutiva

Il progetto è **fattibile e la parte più rischiosa è già risolta.** La tabella di dispatch delle funzioni NWScript di KOTOR 2 è stata **localizzata e verificata byte per byte** su questa macchina: è un array scrivibile di 877 puntatori a funzione, allocato sull'heap e raggiunto da `implementer+0x0C`, con un dispatcher a `0x00668FD0` che fa bounds check contro `0x36D` (877) e restituisce `-2002` per ID fuori range.

Questo cambia la natura del progetto: **non serve fare hijack di funzioni vanilla** (come fa l'extender di KOTOR 1), si può **scrivere direttamente uno slot della tabella** — più economico, per-ID, e componibile.

Restano **tre incognite che vanno chiuse con esperimenti prima di scrivere codice**, tutte da meno di 30 minuti l'una (§7, Q1–Q3). La più importante: sappiamo che il dispatcher rifiuta gli ID ≥ 877, ma **non sappiamo ancora se il chiamante a monte filtra l'ID prima ancora di arrivare al dispatcher**. Se lo facesse, la strategia "appendi ID nuovi" muore e si passa al piano B. Quell'esperimento va fatto per primo.

---

## 1. Posizionamento: cosa esiste davvero

| Progetto | Copre K2? | Cos'è | Rilevanza |
|---|---|---|---|
| **Kotor Patch Manager** (LaneDibello, MIT) | **Sì** | Framework di patching runtime K1+K2. Contiene `AddressDatabases/kotor2_steam_aspyr.db` **con la chiave del binario esatto di questa macchina**, ~45 classi wrapper `GameAPI`, un proxy binkw32 (`KProxy`), script Ghidra | **Il partner naturale.** Il suo Script Extender copre però **solo K1** |
| **K1SE** (`Brotaku-Vengeant`, MIT) | No — solo KOTOR 1 | Proxy `binkw32.dll` + un unico detour MinHook sul dispatcher K1 `0x0052C0D0`; **non registra ID nuovi: fa hijack di 14 ID vanilla** | Utile come *forma* del progetto (build, logging, forwarder). ⚠️ Vedi nota di cautela sotto |
| reone / KotOR.js / xoreos | — | Reimplementazioni open source dell'engine | **Documentazione autorevole** dei formati e delle tabelle routine |

**⚠️ Nota di onestà su K1SE.** Il repo è MIT ma ha 0 star, ultimo push 06/08/2026, e la documentazione ha uno stile che sembra generato da un LLM. Diverse sue affermazioni non sono verificabili (il file `src/hook.cpp` da 117 KB non è stato letto da nessuno nella ricerca; le cifre "4,3M chiamate inoltrate" e "7h10m di stabilità" vivono solo nei commenti). **Trattalo come ispirazione strutturale, non come precedente collaudato**, e verifica ogni sua affermazione prima di dipenderne. Nessuno dei suoi indirizzi funziona su K2 (0 su 4 corrispondenze di byte).

**Conclusione:** la nicchia "script extender maturo per KOTOR 2" è **libera**. Prima di iniziare, però, **contatta LaneDibello** (Discord `@lane_d`): ha già il database di indirizzi per questo binario e nella sua roadmap 2026 c'è "migliore supporto KOTOR 2" più la ricerca di collaboratori. O K2SE diventa la metà-K2 di quell'ecosistema, o duplichi un anno di lavoro in Ghidra.

---

## 2. Fatti verificati sul binario

Tutto quanto segue è stato **verificato leggendo i byte del file** su questa macchina (copia `swkotor2.exe.pre-laa-backup`), non dedotto da fonti terze.

### 2.1 Struttura PE

```
ImageBase   0x00400000        EntryPoint RVA 0x0051D5A2
Characteristics 0x0103        RELOCS_STRIPPED | EXECUTABLE_IMAGE | 32BIT_MACHINE
   (l'exe in uso è 0x0123: ci abbiamo aggiunto LARGE_ADDRESS_AWARE)
.text    VA 0x00401000  vsize 0x5846A8   raw 0x000400
.rdata   VA 0x00986000  vsize 0x06D1E8   raw 0x584C00
.data    VA 0x009F4000  vsize 0x08DF3C   raw 0x5F1E00
.rsrc    VA 0x00A82000  vsize 0x036A10   raw 0x611C00
```

**Nessun packer, nessun wrapper DRM Steam** (raw ≈ virtual su tutte le sezioni, nessuna sezione `.bind`). **`RELOCS_STRIPPED` + niente ASLR ⇒ ogni indirizzo è stabile per sempre, su ogni macchina.** È la proprietà che rende sensata una tabella di offset hardcodata.

### 2.2 La VM: `CSWVirtualMachineCommands`

L'ancora è la stringa RTTI di MSVC, presente in chiaro:

```
.?AVCSWVirtualMachineCommands@@   →  VA 0x00A0F4F8  (file offset 0x60D2F8)   ✅ verificata
```

Da lì si arriva alla vtable, i cui primi quattro slot sono stati letti direttamente dal file:

| Slot | VA | Valore | Cos'è |
|---|---|---|---|
| [0] | `0x009940D0` | `0x00536BE0` | (ctor/dtor scalare) |
| [1] | `0x009940D4` | `0x00665F50` | **`InitializeCommands`** — costruisce la tabella |
| [2] | `0x009940D8` | `0x00668FD0` | **`ExecuteCommand`** — il dispatcher ⭐ |
| [3] | `0x009940DC` | `0x00669020` | (callback engine-structure) |

✅ Tutti e quattro confermati byte per byte.

### 2.3 La tabella: 877 slot, sull'heap

`InitializeCommands` a `0x00665F50`, disassemblato dai byte reali:

```asm
55 8B EC              push ebp; mov ebp,esp
83 EC 0C              sub esp,0Ch
89 4D F4              mov [ebp-0Ch],ecx        ; this
68 B4 0D 00 00        push 0DB4h               ; ⭐ 3508 = 877 × 4 byte
E8 C0 37 2B 00        call operator new[]
83 C4 04              add esp,4
89 45 F8              mov [ebp-8],eax
8B 45 F4              mov eax,[ebp-0Ch]
8B 4D F8              mov ecx,[ebp-8]
89 48 0C              mov [eax+0Ch],ecx        ; ⭐ m_pCommands = this+0x0C
...                                            ; azzera 877 slot, poi:
8B 45 F4  8B 48 0C    mov eax,[ebp-0Ch]; mov ecx,[eax+0Ch]
C7 81 34 01 00 00 A0 C4 68 00
                      mov dword [ecx+134h], 0068C4A0h   ; slot 77 (abs) ← handler
```

**Ogni voce della tabella è popolata da un `mov dword [tabella + ID*4], handler` letterale.** Significa che l'intera mappa `ID → handler` si estrae meccanicamente disassemblando una sola funzione. Coppie decodificate a campione dai byte reali:

| Offset nell'istruzione | Indice = offset/4 | Handler | Nome (xoreos) |
|---|---|---|---|
| `+0x134` | **77** | `0x0068C4A0` | `abs` |
| `+0x11C` | **71** | `0x0068C4A0` | `acos` — *stesso handler matematico* |
| `+0x094` | 37 | `0x0066DAF0` | |
| `+0x7D8` / `+0x7D4` | 502 / 501 | `0x0066E7E0` | |
| `+0xAF0` | 700 | `0x00669910` | |

Il fatto che gli ID 67–77 (`fabs, cos, sin, tan, acos, asin, atan, log, pow, sqrt, abs`) condividano **lo stesso handler** e che i nomi combacino con la lista indipendente di xoreos-tools è la conferma semantica decisiva: la probabilità che sia una coincidenza è nulla.

> **Perché nessuna scansione statica trovava la tabella:** è **allocata sull'heap a runtime**. In `.rdata`/`.data` non c'è nessuna sequenza da 877 puntatori — esiste solo il *puntatore* dentro l'oggetto. Chi cerca "array lunghi di puntatori nelle sezioni" non la troverà mai.

### 2.4 Il dispatcher `ExecuteCommand` — `0x00668FD0`

```asm
55 8B EC              push ebp; mov ebp,esp
83 EC 08              sub esp,8
89 4D F8              mov [ebp-8],ecx          ; this   (__thiscall)
81 7D 08 6D 03 00 00  cmp dword [ebp+8], 36Dh  ; ⭐ nCommandId vs 877
7D 30                 jge  fallimento
8B 45 F8              mov eax,[ebp-8]
8B 48 0C              mov ecx,[eax+0Ch]        ; tabella
8B 55 08              mov edx,[ebp+8]          ; id
83 3C 91 00           cmp dword [ecx+edx*4], 0 ; ⭐ slot NULL?
74 21                 je   fallimento
8B 45 0C  50          push [ebp+0Ch]           ; nParameters
8B 4D 08  51          push [ebp+8]             ; nCommandId
...       FF D0        call eax                ; handler
89 45 FC  8B 45 FC     mov [ebp-4],eax; mov eax,[ebp-4]
EB 05                 jmp  fine
B8 2E F8 FF FF        mov eax, 0FFFFF82Eh      ; ⭐ -2002  (COMMAND_NOT_FOUND)
8B E5 5D C2 08 00     mov esp,ebp; pop ebp; ret 8
```

✅ Confermati: bound `0x36D`, `m_pCommands` a `+0x0C`, stride 4 byte, check NULL per slot, ritorno `-2002`, `__thiscall` con `ret 8`.

**Firma:** `int __thiscall ExecuteCommand(void* this, int nCommandId, int nParameters)`

### 2.5 Altri offset verificati

| Cosa | Dove | Valore |
|---|---|---|
| `m_pCommands` = `+0x0C` | byte a `0x00665F71` e `0x00668FE7` | `0x0C` ✅ |
| `m_pInternal` = `+0x1C` (CVirtualMachine) | byte a `0x006FD9B0` | `0x1C` ✅ |
| Bound routine | dword a `0x00665F87` e `0x00668FDC` | `0x36D` = 877 ✅ |
| Dimensione allocazione | dword a `0x00665F5A` | `0xDB4` = 877×4 ✅ |

---

## 3. Architettura

### 3.1 Vettore di iniezione — `version.dll`, con `binkw32.dll` come fallback

L'exe risolve tutte le DLL **per nome, senza ordinali e senza delay-import**: qualsiasi DLL omonima nella cartella del gioco viene mappata e il suo `DllMain` gira **prima dell'entry point**. Nessuno dei candidati è in `KnownDLLs`, quindi tutti sono ombreggiabili.

| Candidato | Verdetto |
|---|---|
| **`version.dll`** ✅ **scelto** | **Nessun altro modulo nel processo KOTOR 2 lo importa** (superficie reale: 17 export da inoltrare, 3 usati dall'exe). Non è un file del depot Steam ⇒ "Verifica integrità" non lo tocca |
| `binkw32.dll` (fallback) | Convenzione KOTOR, presente nel gioco (347.136 byte). **Ma è un file del depot**: Steam lo ripristina. Richiede rinomina dell'originale in `binkw32Hooked.dll` e l'inoltro di **84** export |
| `opengl32.dll` | 🔴 **Escluso.** Slot conteso da Mesa, ReShade, ShaderOverride e da una mod texture del Workshop. Collisione frontale garantita |
| `dinput8.dll` | 🔴 Escluso: è lo slot di fallback documentato di ReShade |
| `glu32.dll` | 🔴 Escluso: 12 funzioni e dipendenza circolare con opengl32 |
| `winmm.dll` | 🔴 Escluso: altri quattro moduli del processo lo importano ⇒ 40 funzioni da inoltrare |

**Niente launcher.** `swkotor2.exe` non ha `SteamAPI_RestartAppIfNecessary` né `steam_appid.txt`: un launcher che avvia l'exe direttamente perde Workshop, achievement e cloud. Il proxy funziona **chiunque** lanci il gioco — è il maggior vantaggio ergonomico di K2SE su SKSE.

**Caveat Proton/Linux:** su Proton un `version.dll` nella cartella è inerte senza `WINEDLLOVERRIDES="version=n,b"`. Spedisci la build `binkw32` come default per Proton, e **non mettere un VERSIONINFO con CompanyName Microsoft**.

### 3.2 Punto di aggancio — sostituzione dello slot di vtable

**Scrivi `0x009940D8`** (vtable slot [2]) facendolo puntare al thunk di K2SE, salvando l'originale `0x00668FD0` come trampolino.

Perché non un detour inline: lo slot è **un solo dword**, l'installazione è atomica, la disinstallazione è banale, e non serve né MinHook né un trampolino generato. `.rdata` è read-only (`0x40000040`) ⇒ serve `VirtualProtect`, che è nella lista MSDN delle chiamate sicure da `DllMain`.

```c
// pseudocodice del thunk
int __fastcall K2SE_ExecuteCommand(void* self, void* /*edx*/, int id, int nParams)
{
    if (id >= K2SE_FIRST_ID && id < K2SE_FIRST_ID + g_nExtended)
        return K2SE_DispatchExtended(self, id, nParams);
    if (id == 77)                                  // sentinella abs() — §3.5
        return K2SE_ProbeHost(self, id, nParams);
    return g_originalExecuteCommand(self, id, nParams);   // pass-through fedele
}
```

**Fallback se lo slot non fosse l'unico percorso di dispatch:** detour inline MinHook v1.3.4 su `0x00668FD0` (prologo `55 8B EC 83 EC 08`, 6 byte, splice-safe). L'evidenza statica è favorevole — solo due riferimenti a `0x009940D0` in `.text`, entrambi store del vptr, e zero `call` dirette a `0x00668FD0` — ma va **dimostrato** con l'esperimento Q4.

### 3.3 Registrazione delle nuove routine — piano A / B / C

**Piano A (preferito): appendere ID da 877 in su.**
La tabella è un array reale e scrivibile. K2SE intercetta gli ID ≥ 877 nel thunk e li serve dal proprio registro, senza toccare la tabella del gioco. Costo: zero modifiche all'engine.

⚠️ **Condizione di validità:** che il chiamante a monte del dispatcher non filtri l'ID *prima*. Il bound a `0x36D` che conosciamo è dentro `ExecuteCommand`; se ne esistesse un secondo nel chiamante (`ExecuteCode`), l'ID 877 non arriverebbe mai. **Questo è Q3, l'esperimento da fare per primo.**

**Piano B: allargare le tre costanti.** Se il filtro a monte esiste ma confronta contro lo stesso 877, si allargano i tre immediati (`0x00665F59` allocazione, `0x00665F87` bound di init, `0x00668FDC` bound del dispatcher) e si aggancia anche lo slot [1] per riempire le voci nuove.

**Piano C: hijack di ID vanilla** (il modello K1SE), con reimplementazione fedele della routine dirottata. Brutto semanticamente e costoso, ma il meccanismo della sentinella `abs()` (§3.5) dimostra già che funziona ed è generalizzabile a qualunque routine.

### 3.4 API per i mod author

Il modder include un header e chiama funzioni normali:

```c
#include "k2se"

void main() {
    if (K2SE_Version() == 0) return;          // K2SE assente → degrada in silenzio
    K2SE_SetAreaFog(GetArea(OBJECT_SELF), TRUE, 20.0, 120.0, 0x402010);
}
```

Convenzioni: include `k2se.nss`, prefisso `K2SE_` su ogni funzione, versione codificata `major*10000 + minor*100 + patch` (mai 0 — 0 significa "assente"). **Pubblica fin dal primo giorno un registro pubblico degli ID allocati**: con 877 ID vanilla e un solo spazio condiviso, due extender che collidono sullo stesso slot sono un guasto irrecuperabile.

**Trappola da documentare in grassetto:** `nwnnsscomp` risolve i nomi in ID **per posizione** nel `nwscript.nss`. Un header mancante ricade sulla tabella vanilla **senza alcun errore**, e l'NCS non ha checksum — quindi lo sbaglio si manifesta solo a runtime.

### 3.5 Rilevare K2SE senza rischi: la sentinella `abs()`

Il problema: uno script che chiama una funzione K2SE su una macchina senza K2SE **non può usare quella chiamata per accorgersene** — la chiamata *è* il guasto (ritorna `-2002`, e cosa ne faccia la VM è ancora ignoto).

La soluzione elegante: **`abs()` è la routine vanilla 77**, dichiarata in ogni `nwscript.nss` non modificato. K2SE la intercetta e la reimplementa fedelmente, con un solo ramo in più:

```c
static int H_ProbeHost(void* vm, int id, int nParams)      // id == 77
{
    int v;
    if (nParams != 1)              return g_originalExecuteCommand(vm, id, nParams);
    if (!VM_StackPopInteger(&v))   return K2SE_PARAM_ERROR;
    if (v == K2SE_PROBE_MAGIC)   { VM_StackPushInteger(K2SE_VERSION_ENCODED); return 0; }
    VM_StackPushInteger(v < 0 ? -v : v);                    // abs() fedele
    return 0;
}
```

Con `K2SE_PROBE_MAGIC = -1234567890`: **senza** K2SE `abs()` restituisce `1234567890`, **con** K2SE restituisce il codice di versione. Costa **zero ID nuovi**, non richiede l'header esteso, e funziona su un gioco vanilla. `abs` è la scelta giusta perché è puro, senza parametri di default (argc sempre 1), quindi la reimplementazione è bit-identica.

### 3.6 Fingerprint e fail-safe

**Mai un hash dell'intero file**: l'exe di questa macchina è già LAA-patchato (`0x0123` invece di `0x0103`) e un hash globale rifiuterebbe l'installazione dell'utente stesso. Verifica invece: TimeDateStamp `0x5603005D`, layout delle sezioni, i valori della vtable, e hash FNV-1a-32 su brevi span di codice (dispatcher, init, stack accessor) — **mascherando esplicitamente il campo Characteristics** e accettando sia `0x0103` sia `0x0123`.

Verifica anche gli **offset di struttura**, che K1SE dichiara essere il suo anello debole: qui sono tutti derivabili dai byte delle istruzioni che li usano (il `0x0C` a `0x00665F71`, il `0x1C` a `0x006FD9B0`, il `0x36D`, il `0xDB4`).

**Auto-validazione a runtime**, la miglior asserzione disponibile — alla prima chiamata di `ExecuteCommand`, con `this` da ECX:

```c
void** cmds = *(void***)((char*)self + 0x0C);
assert(cmds[0]   == (void*)0x0068F5D0);   // Random
assert(cmds[77]  == (void*)0x0068C4A0);   // abs   ← verificato staticamente
assert(cmds[876] == (void*)0x0069C460);   // RebuildPartyTable
```

Dimostra in un colpo solo il layout dell'oggetto, che la tabella è popolata, e che la mappa ID→handler è quella ricostruita.

**Postura in caso di mismatch: rifiuta di installare.** Un hook presente ma inerte è peggio di nessun hook. E siccome un rifiuto silenzioso è indistinguibile dall'assenza, **segnalalo a schermo** con `AurPostString` (`0x00474C00`): trasforma i tre bug report più probabili (conflitto di wrapper / Steam ha ripristinato il proxy / build non riconosciuta) in una diagnosi a colpo d'occhio.

### 3.7 Logging

- Destinazione `%LOCALAPPDATA%\K2SE\k2se.log` — **mai la cartella del gioco**.
- Logga **tutte le scritture E tutti i rifiuti di scrivere**: un rifiuto è l'*assenza* di una scrittura, quindi la regola "logga le scritture" da sola manca esattamente il caso di guasto silenzioso.
- Usa Win32 grezzo (`CreateFileW`/`WriteFile`/`CloseHandle`), niente CRT: all'uscita del processo Windows termina gli altri thread **prima** di `DLL_PROCESS_DETACH`, e un thread ucciso mentre teneva il lock del CRT causerebbe un deadlock.
- Tracing verboso attivato da un **file marker** accanto alla DLL, mai da una variabile d'ambiente e **mai da una funzione chiamabile dagli script** (sarebbe una scrittura su stato globale che una mod potrebbe infliggere alla macchina altrui).
- Blocco di identità in testa a ogni run: SHA-256 dell'exe, Characteristics, esito per singola probe, e **censimento dei conflitti**: quali fra `opengl32.dll` / `dinput8.dll` / `binkw32Hooked.dll` sono presenti nella cartella.

---

## 4. Piano di reverse engineering

### 4.1 Ancore, in ordine di efficacia

| # | Ancora | Selettività | Verdetto |
|---|---|---|---|
| **1** | **Stringhe RTTI MSVC** (`\.\?A[VU][\w@?$]{2,90}@@`) | 389 totali, 3 rilevanti | ⭐ **La migliore.** Ghidra le sfrutta da solo: l'analizzatore "Windows x86 PE RTTI Analyzer" etichetta la vtable a `0x009940D0` **senza lavoro manuale** |
| **2** | Costante di allocazione `push 0DB4h` (`68 B4 0D 00 00`) | **1 sola occorrenza** in 6,5 MB | Eccellente e generalizzabile: per qualunque build Odyssey, calcola N×4 dal `nwscript.nss` corrispondente |
| **3** | Il conteggio come imm32 (`6D 03 00 00`) | 9 hit, 2 reali | Buona con triage. ⚠️ Cerca il **dword grezzo**: la forma registro (`3D`, `81 F8`) dà **zero** hit perché MSVC confronta contro slot di stack |
| 4 | ResType NCS = 2010 (`0x7DA`) | 3 immediati | Indiretta |
| 5 | Stringhe GFF (`SunFogFar`, `FieldOfView`) | molto selettive | ❌ Inutili per la VM — portano al parser delle aree. ⭐ **Ma sono l'ancora migliore per la nebbia** (§5, M5) |
| — | Magic `"NCS V1.0"` | **0 occorrenze** | ❌ Vicolo cieco, non costruirci sopra |
| — | Firme di K1SE | **0 occorrenze** | ❌ Nessun byte di K1SE si trasferisce |

### 4.2 Verifica dinamica in x32dbg (~20 minuti, da fare prima di scrivere codice)

```
bp 0x00665F50     ; InitializeCommands — ECX = this; conferma [this] == 0x009940D0
                  ;   dopo 0x00665F6F: *(this+0x0C) == EAX (blocco heap da 0xDB4)
bp 0x00668FC1     ; chiamata all'installer dei minigiochi
                  ;   PRIMA: dump 877 dword → 103 zeri;  DOPO: zero NULL
bp 0x00668FD0     ; ExecuteCommand — ECX=this, [esp+4]=id, [esp+8]=nParams; ret 8
bp condizionale [esp+4]==77   ; abs → conferma che il callee è 0x0068C4A0
bp 0x006FD9A0 / 0x006FD9C0    ; StackPopInteger / StackPushInteger (contratto EAX)
hw write bp 0x009940D8        ; per una sessione intera: nessun altro scrive lo slot
```

### 4.3 Setup Ghidra

1. Importa **`swkotor2.exe.pre-laa-backup`** (la copia pristina — è quella a cui è agganciato il DB di KPM). Linguaggio `x86:LE:32:default`, compilatore `windows`, ImageBase `0x00400000`.
2. Auto-analisi con **RTTI Analyzer attivo** (default) → la vtable viene etichettata da sola.
3. Importa i nomi di funzione dal `kotor2_steam_aspyr.db` di KPM.
4. Applica la tabella 877 come label, nominando gli handler da `xoreos-tools/game_kotor2.h`. **Prendi l'ultima scrittura per indice** (lo slot 156 è scritto due volte).
5. Definisci le struct `CSWVirtualMachineCommands { +0x00 vptr; +0x0C void** m_pCommands; }` e imposta la firma di `0x00668FD0`.

---

## 5. Milestone

### M0 — Riprodurre i fatti e preparare la toolchain
**Obiettivo:** ri-derivare da solo ogni indirizzo, così nulla in questo documento è preso per fede.

- [ ] Ghidra etichetta la vtable a `0x009940D0` dall'RTTI, con slot[1]=`0x00665F50`, slot[2]=`0x00668FD0`
- [ ] `68 B4 0D 00 00` compare **una volta sola**, a VA `0x00665F59`
- [ ] Uno script che parsa `InitializeCommands` recupera **877/877** slot, e ogni handler distinto inizia con `55 8B EC`
- [ ] `cmd[0]==0x0068F5D0`, `cmd[77]==0x0068C4A0`, `cmd[876]==0x0069C460`
- [ ] **Q1 e Q2 risolti** (vedi §7) — costano 20 minuti e fanno da cancello all'intero design
- [ ] CMake + VS2022 producono una DLL 32-bit con CRT statico che importa **solo** KERNEL32

*Da installare:* Visual Studio 2022 Community (workload "Sviluppo desktop con C++", toolset x86), CMake, Ghidra + JDK 21, x32dbg. Hai già git, Python 3.12 e Cheat Engine.

### M1 — Il proxy si carica e logga
**Obiettivo:** `version.dll` viene mappata, inoltra tutti e 17 gli export, scrive il log — con il gioco **identico al 100%**.

- [ ] `dumpbin /exports` sulla DLL costruita produce la stessa lista di `SysWOW64\version.dll` (verifica automatica in build, non manuale)
- [ ] Il gioco parte da Steam, arriva al menu, riproduce un video Bink con audio, l'overlay Steam si apre
- [ ] Il log contiene il blocco identità e il censimento dei conflitti
- [ ] Rimuovendo la DLL si torna esattamente al vanilla

**Rischio:** qualcosa nell'ambiente utente (antivirus, RivaTuner, overlay Discord/AMD) importa `version.dll` e ha bisogno di un export che K2SE ha stubbato male → **inoltrali tutti e 17**, non solo i 3 che servono all'exe.
**Piano B:** cambia slot a `binkw32` (rinomina l'originale, inoltra 84 export).
**Può uccidere il progetto?** No. Il caso peggiore è un cambio di slot.

### M2 — Hook installato, pass-through trasparente dimostrato
- [ ] Il log registra: fingerprint OK, valore vecchio `0x00668FD0`, nuovo = thunk, protezione vecchia `PAGE_READONLY`
- [ ] L'auto-validazione a runtime passa alla prima chiamata
- [ ] Un contatore di chiamate inoltrate raggiunge ≥ 1.000.000 in una sessione, senza crash né differenze di comportamento (usa `lock inc` e salva i flag: **la VM non è l'unico thread del processo**)
- [ ] **Test differenziale:** stessa sequenza scriptata (dialogo, combattimento, una corsa swoop, un minigioco con torretta) identica con e senza K2SE
- [ ] `DLL_PROCESS_DETACH` ripristina il dword originale

**Rischio killer:** la vtable non è l'unico percorso di dispatch → **Q4**.
**Piano B:** detour inline MinHook su `0x00668FD0`.

### M3 — Segnale di presenza in gioco (sentinella `abs()`) ✅ CHIUSA 27/08/2026
- [x] `abs(-1234567890)` risponde il codice di versione (100) con K2SE — log: `PRESENCE PROBE answered with version 100`, `TEST 2 PASS`
- [x] `abs(-5)==5`, `abs(7)==7`, `abs(0)==0` con la reimplementazione K2SE attiva (`TEST 3/4/5 PASS`)
- [x] ID 67–76 (stesso handler matematico) intatti: mai intercettati (`Intercepts()` risponde solo all'ID 77) e la sessione di 32.763 dispatch è passata pulita
- [ ] *(non ancora provato)* il ramo "senza K2SE → 1234567890" su un'installazione vanilla — banale, richiede solo una run senza la DLL

**Può uccidere il progetto?** No — è indipendente da Q3 e funziona anche se gli ID estesi si rivelassero irraggiungibili.

### M4 — Prima routine nuova all'ID 877 ⚠️ **LA MILESTONE CHE DEFINISCE IL PROGETTO**
- [ ] L'`nwscript.nss` esteso (877 vanilla + 1) compila senza errori
- [ ] L'hex dump del `.ncs` mostra i byte ACTION **`05 00 03 6D 00`** (opcode `0x05`, routine `0x036D` = 877, argc 0)
- [ ] **Test di regressione:** una chiamata a `GetItemComponent()` emette ancora `05 00 03 03 00` (771) — se il compilatore avesse saltato in silenzio la dichiarazione malformata della routine 767, tutto da 768 in su sarebbe sfalsato di uno e questo test lo cattura subito
- [ ] In gioco lo script mostra la versione via `AurPostString`
- [ ] Un ID esteso non registrato (878) ritorna `-2002` e si comporta come senza K2SE

**Rischio killer:** la VM filtra l'ID prima del dispatcher (**Q3**) → piani B/C di §3.3.

### M5 — Prima feature reale: nebbia/atmosfera a runtime
- [ ] `K2SE_SetAreaFog(oArea, TRUE, 20.0, 120.0, 0x402010)` cambia visibilmente la nebbia
- [ ] La rilettura restituisce ciò che è stato scritto; niente regressioni nelle aree non toccate

⚠️ **Scoperta che cambia il piano:** sulla build Aspyr i *fragment program* ARB **non leggono mai `state.fog`**. Scrivere lo stato GL della nebbia non produce **nessun** effetto visibile — ed è *questo* il vero bug della nebbia Aspyr. Il 3C-FD Patcher lo risolve riscrivendo gli shader.

Conseguenze di design:
- **Non duplicare né combattere 3C-FD.** Rilevalo all'avvio (le sue sei stringhe di ricerca stanno a VA note) e classifica l'exe come stock / patchato / ignoto, degradando con un messaggio chiaro.
- Il meccanismo primario è **hookare `glFogf`/`glFogfv`/`glFogi` via IAT** (slot `0x00986394` / `0x00986398` / `0x009863B0`): qualunque cosa faccia l'engine, i valori di K2SE vincono. Non serve nessuna relocazione — cosa che conta, visto che l'exe è `RELOCS_STRIPPED`.
- Opzione forte: hookare `glProgramStringARB` e riscrivere il sorgente ARB al volo, applicando in memoria l'equivalente della patch 3C-FD. Zero modifiche su disco all'exe.
- **Niente `SetRoomFog`** — la nebbia è solo per-area. Niente parametro densità: la nebbia è **lineare** e la densità non verrebbe mai letta. Esporre invece le varianti Sun/Moon, o si viene sovrascritti alla transizione giorno/notte.
- La nebbia è calcolata **per vertice** e interpolata: su stanze con poligoni grandi e piatti si vedrà banding. Aspettative da tarare.
- **Draw distance:** non esiste un campo far-plane. L'unica leva sarebbe hookare `gluPerspective` (IAT `0x00986028`), con rischio concreto di rompere il culling VIS e gli skybox. **Fuori dall'MVP.**

---

## 6. Adozione — il criterio di successo vero

Il criterio non è "una funzione nuova gira", è **"almeno una mod pubblicata dipende da K2SE"**. Il precedente è istruttivo: lo Script Extender di KPM per K1 funziona da un anno e ha **zero mod dipendenti**.

Piano concreto: scegli 2–3 problemi NWScript **nominati da mod author veri** su Deadly Stream, implementa esattamente quelli, e fai entrare una mod che li usa nella build di kotor.neocities.org.

**Distribuzione:** repo GitHub MIT come fonte di verità (stessa licenza di entrambi i precedenti); voce su Deadly Stream sotto *Modding Tools*; thread `TOOL:K2SE — KOTOR 2 Script Extender`. Nexus opzionale. **Niente Steam Workshop:** gli item del Workshop vivono in `steamapps/workshop/content/208580/<id>` e sono solo percorsi di ricerca risorse — una DLL non può finire accanto all'exe da lì.

L'installazione va resa esprimibile con gli strumenti che i modder già usano: un gruppo `[InstallList]` con destinazione `.` può legittimamente depositare file accanto all'exe, ma `changes.ini` **non ha una primitiva di rinomina** — quindi la rinomina di binkw32 richiede un installer tuo o un frammento KOTORModSync (che supporta rename e run-command). **Pubblica tu quel frammento**, così gli autori di mod build non devono scriverlo.

---

## 7. Domande aperte e l'esperimento più economico per ciascuna

### ✅ RISOLTE il 26/08/2026 — Q1, Q2, Q3, Q4, Q5

**Q1 e Q2 — il compilatore accetta l'header esteso? RISOLTO: sì, entrambe.**
Test eseguito con il vero `nwnnsscomp.exe` (dal repo di KotOR Scripting Tool) e l'`nwscript.nss` di TSL.

L'header contiene esattamente **877 prototipi**, il primo `Random` (id 0) e l'ultimo `RebuildPartyTable` (id 876) — che coincidono con gli handler letti dal binario a `cmds[0]` e `cmds[876]`. Conferma incrociata indipendente del conteggio.

*Q2 (nessuna rinumerazione):* compilando una chiamata a `GetItemComponent()`, che nell'header sta in posizione 771, il compilatore emette `05 00 03 03 00` — **id 771**. Nessuno slittamento. Il timore sulla "routine 767 malformata" era infondato: la 767 è `void SetAvailableNPCId(int nNPC, object oidNPC)`, perfettamente valida.

*Q1 (header esteso):* appendendo `int K2SE_GetVersion();` dopo l'ultima routine e compilando una chiamata, il compilatore emette:

```
05 00 03 6d 00
│  │  └──┬─┘ └── argc = 0
│  │     └────── routine 877 (0x036D, big endian)
│  └──────────── type
└─────────────── opcode ACTION
```

**Identico byte per byte a quanto previsto.** `nwnnsscomp` non ha alcun tetto sugli ID e li assegna per posizione, quindi l'header esteso funziona senza modificare il compilatore.

Con Q1, Q2 e Q3 risolte, **tutti i prerequisiti di M4 sono soddisfatti**: il compilatore produce l'ID 877, l'interprete non lo filtra, e K2SE lo intercetta. Resta solo da far eseguire al gioco uno script compilato.

### ✅ RISOLTE il 26/08/2026 — Q3, Q4, Q5

**Q5 — contratto degli accessor dello stack. RISOLTO per disassemblaggio.**
La verità sta nell'handler matematico vanilla a `0x0068C4A0`, che implementa `abs` (routine 77):

```asm
cmp  dword ptr [ebp+8], 4Dh      ; nCommandId == 77
lea  eax, [ebp-4]                ; &out
push eax
mov  ecx, dword ptr [0xA1B4A8]   ; ECX = il singleton CVirtualMachine
call 0x006FD9A0                  ; StackPopInteger -> EAX != 0 = successo
test eax, eax
jne  ok
mov  eax, 0FFFFF82Fh             ; -2001 se il pop fallisce
...
push edx                         ; il risultato
mov  ecx, dword ptr [0xA1B4A8]
call 0x006FD9C0                  ; StackPushInteger
mov  eax, 0FFFFF830h             ; -2000 se il push fallisce
```

- `int __thiscall StackPopInteger(void* vm, int* out)` — `ret 4`, EAX≠0 = successo
- `int __thiscall StackPushInteger(void* vm, int value)` — `ret 4`, EAX≠0 = successo
- **Correzione al design:** il puntatore alla VM viene da un **globale a `0x00A1B4A8`**, non da `self+0x1C` come avevo assunto. Il `+0x1C` è usato un livello più in basso, *dentro* l'accessor (`mov ecx,[ecx+1Ch]`), non dal chiamante.
- Codici d'errore completi: `-2000` push fallito, `-2001` pop fallito, `-2002` comando inesistente.

`K2SE_ENABLE_STACK_ABI` è ora a `1`.

**Q4 — la vtable è l'unico percorso di dispatch? RISOLTO: sì.**
Zero `call` dirette a `0x00668FD0` in tutto `.text`; solo due riferimenti alla vtable `0x009940D0`, entrambi store del vptr nel costruttore. E l'hook funziona in gioco.

**Q3 — c'è un secondo bounds check a monte? RISOLTO: NO. La strategia "appendi ID" è valida.**

Il chiamante è stato individuato facendo registrare a K2SE il proprio indirizzo di ritorno in gioco: `0x0070029B`. La funzione che lo contiene è il caso ACTION dell'interprete NCS:

```asm
; ID routine: 16 bit BIG-ENDIAN dal bytecode
movsx edx, byte [ecx+eax+2]      ; byte alto
shl   edx, 8
movsx ecx, byte [eax+ecx+3]      ; byte basso
add   edx, ecx                   ; routineId = (b2<<8) | b3
mov   word [ebp-74h], dx         ; conservato come WORD
movsx edx, byte [ecx+eax+4]      ; argc
mov   byte [ebp-69h], dl

cmp   dword [eax+3D4h], 0        ; <-- UNICO test: l'oggetto commands esiste?
jne   ok                         ;     NON e' un controllo di range
mov   eax, 0FFFFFF94h            ; -108 se manca

ok: movzx ecx, byte [ebp-69h]    ; argc
    push  ecx
    movsx edx, word [ebp-74h]    ; routineId, ESTESO CON SEGNO da 16 bit
    push  edx
    mov   ecx, [eax+3D4h]        ; l'oggetto CSWVirtualMachineCommands
    mov   edx, [ecx]             ; vptr
    mov   eax, [edx+8]           ; slot 2 = ExecuteCommand
    call  eax                    ; <-- il nostro hook
    mov   [ebp-70h], eax
    cmp   dword [ebp-70h], 0
    jge   continua               ; >= 0 -> prosegue
    mov   eax, [ebp-70h]
    jmp   0x701E58               ; < 0 -> ABORTA lo script propagando il codice
continua:
    add   edx, 5                 ; pc += 5 (ACTION e' lunga 5 byte)
```

Tre conclusioni operative:

1. **Nessun controllo di range prima del dispatcher.** Fra la lettura dell'ID e la chiamata c'è un solo test, e verifica che l'oggetto commands non sia NULL. L'ID 877 arriva al nostro hook.
2. **L'ID è un intero a 16 bit con segno** (`movsx ... word`): lo spazio utilizzabile arriva fino a **32767**, non si ferma a 877. Spazio abbondante per gli ID estesi.
3. **Degradazione senza K2SE, ora nota con precisione:** un ritorno negativo fa `jmp` all'uscita propagando il codice — cioè **aborta lo script corrente**, senza far crashare il gioco. Uno script compilato contro l'header esteso, eseguito senza K2SE, muore alla prima chiamata estesa. Da documentare in grassetto per i modder, e ragione in più per usare sempre `K2SE_IsPresent()` per primo.

Confermata anche la codifica ACTION: opcode a `+0`/`+1`, **routine ID big-endian a `+2`/`+3`**, argc a `+4`, istruzione lunga 5 byte. Cioè `05 00 03 6D 00` per la routine 877 con 0 argomenti — esattamente quanto previsto per M4.

---

### ✅ RISOLTE il 26/08/2026 (sera) — Q6, Q7 + l'ABI completa dello stack

**Q6 — l'ordine di pop coincide con l'ordine di dichiarazione? RISOLTO: SÌ, staticamente.**
La prova è il ramo `pow` dell'handler matematico (routine 75, non commutativa): il **primo** float poppato (`[ebp-0xC]`, nel preambolo comune) è quello che l'handler passa alla CRT `pow()` come **base**, cioè `fValue`, il **primo** parametro dichiarato; il secondo pop (`[ebp-0x10]`, dentro il ramo) diventa l'esponente. Conferma incrociata dal lato compilatore: nel bytecode gli argomenti sono pushati in ordine **inverso** (l'ultimo dichiarato per primo), quindi il primo dichiarato è in cima allo stack al momento della ACTION. **CONFERMATO END-TO-END IN GIOCO il 27/08/2026**: la routine di test 878 (`K2SE_SelfTest(111, 2.5, 333)`) ha poppato `111, 2.5, 333` in quest'ordine e restituito il checksum esatto `111250333` per **292 chiamate su 292**, zero errori.

**Q7 — argomenti di default omessi? RISOLTO: il compilatore li materializza LUI.**
`GetIsInCombat()` (2 parametri, entrambi default) emette `argc=2`, e il bytecode contiene `CONSTO OBJECT_SELF` + `CONSTI 0` generati dal compilatore. Vale anche per l'header esteso: `K2SE_Q7Probe(int nA, int nB = 7)` chiamata come `K2SE_Q7Probe(1)` emette `argc=2` con un `CONSTI 7` visibile nel bytecode. **Conseguenza: le routine K2SE possono dichiarare default liberamente; l'handler vede sempre l'argc pieno dichiarato.** Un argc diverso da quello dichiarato = script compilato contro un header sbagliato → K2SE rifiuta con `-2001` senza toccare lo stack. Test: `tools/q7_default_args_test.py`.

**ABI stack completa (int/float/object/vector), letta dagli handler vanilla:**

| Accessor | VA | Firma (`__thiscall`, ECX=VM da `[0x00A1B4A8]`, EAX≠0 = successo) |
|---|---|---|
| StackPopInteger | `0x006FD9A0` | `(vm, int* out)` ret 4 |
| StackPushInteger | `0x006FD9C0` | `(vm, int value)` ret 4 |
| StackPopFloat | `0x006FD9E0` | `(vm, float* out)` ret 4 — dal preambolo del math handler |
| StackPushFloat | `0x006FDA00` | `(vm, float value)` ret 4 — dall'epilogo comune del math handler |
| StackPopVector ⚠️ | `0x006FDA20` | `(vm, float out[3])` ret 4 — **solo forma, nessun handler letto** |
| StackPushVector ⚠️ | `0x006FDA40` | `(vm, float x, float y, float z)` ret 0xC — **solo forma** |
| StackPopObject | `0x006FDAF0` | `(vm, uint32* out)` ret 4 — da GetArea (id 24) |
| StackPushObject | `0x006FDB10` | `(vm, uint32 objId)` ret 4 — da GetFirstPC (id 548) |

Bonus dagli stessi handler: **`OBJECT_INVALID = 0x7F000000`** (valore di default/fallimento in GetFirstPC); `[0x00A1B4A4]` è un secondo singleton (app/server) da cui gli handler raggiungono gli oggetti di gioco; la trigonometria NWScript **converte i gradi in radianti internamente** (`fmul` per π/180 prima della CRT).

**AurPostString VERIFICATA:** `void __cdecl AurPostString(const char* text, int x, int y, float fLife)` a `0x00474C00` — `ret` liscio (cdecl), quattro argomenti, float per valore; alloca un oggetto testo da 0x434 byte e lo costruisce via `0x004744D0`. Il call site engine `0x0040820A` passa un `char*` grezzo verso `.rdata` (`">"`, 5.0f) ⇒ è una C string, **non** una CExoString. Il banner K2SE usa un buffer statico perché non è provato se l'engine copi la stringa o tenga il puntatore.

Tutti e cinque i nuovi indirizzi hanno probe nel fingerprint (rel32 dei call site verificati, non i primi byte del callee): 15/15 OK a runtime sull'exe LAA-patchato.

### Domande ancora aperte

| # | Domanda | Esperimento | Costo | Blocca |
|---|---|---|---|---|
| **Q8** | Steam "Verifica integrità" lascia stare una `version.dll` aggiunta e ripristina `binkw32.dll`? ⚠️ *la verifica ripristina anche l'exe LAA-patchato: farla solo con `Patch-KOTOR2-LAA.ps1` alla mano* | Esegui la verifica col proxy installato | 15 min | scelta dello slot + policy LAA |
| **Q9** | Qualcosa nell'ecosistema KOTOR 2 occupa già `version.dll`? | Chiedi su Deadly Stream / Discord prima di bloccare lo slot | 1 giorno | scelta dello slot |
| **Q10** | Il depot Aspyr riceve ancora aggiornamenti? | Storico depot su SteamDB per l'app 208580 | 5 min | rischio dello slot binkw32 |
| **Q11** | ABI di CExoString (chi possiede/libera il buffer di StackPopString/PushString?) e degli engine structure (location/effect/talent) | Leggi un handler che consuma stringhe (es. `PrintString`) e uno che consuma location | 45 min | routine con parametri stringa/location |

---

## 8. Registro delle correzioni — cose da NON ripetere

Errori documentati emersi durante la ricerca; sono qui perché ricompaiono facilmente.

1. **`-2001`/`-2002` non sono strref di `dialog.tlk`.** Nel TSL sono entrambe stringhe vuote. Sono codici interni opachi.
2. **Il "secondo installer" non è late binding.** `0x006F5B80` ha un solo chiamante, l'ultima istruzione prima dell'epilogo di `InitializeCommands`. La finestra con 103 NULL **non esiste mai a runtime**: non usarla per giustificare un'architettura a registrazione dinamica.
3. **Il primo ID libero è 877, non 772.** 772 / `0x304` sono valori di KOTOR 1.
4. **"Non c'è tabella da patchare, quindi devi fare hijack" è FALSO per K2.** La tabella esiste, è reale e scrivibile.
5. **"Uno slot non implementato ritorna 0" è FALSO per K2.** Tutti gli 877 slot sono popolati; fuori range ritorna `-2002`.
6. **Il catalogo di funzioni di K1SE è un backport TSL→K1 e non serve come lista feature per K2.** `GrantFeat`, `GetFeatAcquired`, `AdjustCreatureSkills`, `ModifyReflexSavingThrowBase` ecc. **esistono già nativamente in K2**.
7. **La logica "hook differito per SteamStub" non si applica:** `swkotor2.exe` è in chiaro e non impacchettato.
8. **`0x0085CE5D` non è un hook site valido** per la build Steam-Aspyr: è una copia GOG non portata di KPM, e i suoi byte non esistono in questo binario.
9. **`nwnsc` non è il compilatore di KOTOR.**
10. **`NWNXLib` non è una guida agli offset:** in NWN `+0x0C` è `m_pVM`, in KOTOR è l'array dei comandi.

---

## 9. Link operativi

**Reverse engineering** — Ghidra https://ghidra-sre.org/ · x32dbg https://x64dbg.com/ · MinHook v1.3.4 (tag reale) https://github.com/TsudaKageyu/minhook

**Prior art KOTOR** — Kotor Patch Manager https://github.com/LaneDibello/Kotor-Patch-Manager (guarda `AddressDatabases/kotor2_steam_aspyr.db`, `Patches/Common/GameAPI/`, `src/KProxy/`) · K1SE https://github.com/Brotaku-Vengeant/Kotor-Script-Extender-Public- · thread RE https://deadlystream.com/topic/11948-kotor-1-gog-reverse-engineering/

**Tabelle routine (validazione incrociata indipendente)** — xoreos-tools `game_kotor2.h` https://github.com/xoreos/xoreos-tools/blob/master/src/nwscript/game_kotor2.h (877 nomi, ID 0–876) · reone https://github.com/seedhartha/reone · KotOR.js `NWScriptDefK2.ts` https://github.com/KobaltBlu/KotOR.js · PyKotor `scriptdefs.py` https://github.com/NickHugi/PyKotor

**Formato NCS** — spec Torlack https://github.com/xoreos/xoreos-docs/blob/master/specs/torlack/ncs.html

**Tecnica proxy DLL** — ordine di ricerca DLL https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order · best practice DllMain https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices · DLL proxying https://itm4n.github.io/dll-proxying/ · Ultimate ASI Loader https://github.com/ThirteenAG/Ultimate-ASI-Loader

**Architettura Aurora (solo concettuale — NWN:EE, non Odyssey)** — NWNX:EE https://github.com/nwnxee/unified

---

## 10. Prossimo passo

*(aggiornato 26/08/2026 sera — Q1–Q7 tutte risolte, M1–M4 chiuse, ABI int/float/object completa)*

1. ~~**Sessione di gioco di verifica**~~ ✅ **FATTA 27/08/2026 09:20 — TEST 1..6 tutti PASS**, banner a schermo, self-validation OK, 32.763 dispatch, hook rimosso pulito all'uscita. M3 chiusa, Q6 confermata end-to-end, ABI float/object viva. Il marker `K2SE_DIAGNOSTIC` è stato rimosso (log silenzioso per il gioco normale); la batteria heartbeat resta installata come proof-of-life — si toglie con `python tools/m4_deploy_test.py --clean`.
2. **M5 — nebbia/atmosfera a runtime** (§5): ora ha tutti i prerequisiti. Primo sottopasso: rilevare 3C-FD e disassemblare il percorso `glProgramStringARB`/IAT `glFogf`.
3. **Q11** (stringhe/engine structure) quando una feature le richiede — non prima.
4. Prima della prima release pubblica: riordinare l'header esteso (le routine di self-test 878/879 dopo le API vere), contattare LaneDibello, pubblicare il registro degli ID.
