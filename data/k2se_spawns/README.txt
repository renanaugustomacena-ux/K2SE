k2se_spawns -- oggetti e NPC aggiunti alle aree (K2SE spawner)
================================================================

Un file per modulo: <MODULO>.ini (il nome che GetModuleName() restituisce,
per esempio 101PER.ini per la stazione mineraria di Peragus, 003EBO.ini per
l'Ebon Hawk). Ogni sezione con un Template e' una cosa da creare:

    [spawn1]
    Type=placeable          ; oppure creature
    Template=plc_crate      ; resref di un .utp / .utc (386 placeable in templates.bif)
    X=-7.74
    Y=19.95
    Z=9.67
    Facing=90               ; gradi
    Area=101PERa            ; facoltativo: solo in questa area (tag). Senza: tutto il modulo

Come si prendono le coordinate: in gioco, mettiti dove vuoi l'oggetto e premi
F10. K2SE aggiunge un blocco [spawnN] gia' compilato (modulo, area, X Y Z,
Facing) a k2se_spawns\_captured.txt: copialo nel file del modulo e cambia il
Template.

Il file viene riletto a ogni cambio di modulo. Gli oggetti creati sono marcati
(LocalBoolean 150, LocalNumber 25) e salvati con l'area: caricare una partita
non li duplica, e un ingresso in un'altra zona del modulo li rispetta.

Placeable utili (tutti in templates.bif): plc_crate, plc_metalbox, plc_barrel01,
plc_barrel02, plc_footlker, plc_locker01, plc_lockerlg, plc_filecabinet,
plc_light, plc_plant1, plc_plant2, plc_hangplant01, plc_plantpltr01,
plc_bench, plc_bench2, plc_lbench01, plc_chair1, plc_chair2, plc_chair03,
plc_desk1, plc_desk2, plc_gentable, plc_extable01, plc_bed1, plc_bed2,
plc_czsign. NPC generici: n_commm, n_commf, n_fatcomm, n_commkidm, n_commkidf,
g_thug01, g_thug02, g_thug03, g_sithcomm01 (con NpcVariety attivo ricevono
teste diverse da soli).

Se un oggetto non appare: k2se.log riporta "spawner: [spawnN] ... FAILED"
(template inesistente o punto fuori dal walkmesh). Dopo 3 tentativi si arrende.
