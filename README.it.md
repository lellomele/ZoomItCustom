# ZoomIt Custom

[English](README.md)

ZoomIt Custom è un'app portatile per Windows per ingrandire lo schermo, disegnare, mostrare un timer di pausa e catturare ritagli. La versione Custom nasce per risolvere bug storici di ZoomIt, soprattutto la scomparsa del puntatore del mouse nei passaggi tra LiveZoom e disegno, e migliorare robustezza, velocità di risposta e occupazione di memoria.

Registrazione video/audio, Type e DemoType sono escluse per concentrare l'app su questi strumenti di uso quotidiano.

## Per iniziare

Scaricare ed estrarre un [pacchetto portatile dalle release](https://github.com/lellomele/ZoomItCustom/releases) che contenga entrambi gli eseguibili. Se un pacchetto con il supervisore non è ancora disponibile, seguire le istruzioni di compilazione qui sotto. Tenere i due eseguibili nella stessa cartella e chiudere altre istanze di ZoomIt che utilizzano le stesse scorciatoie.

| Cosa vuoi fare | Cosa avviare |
| --- | --- |
| Usare l'app con le protezioni interne | ZoomItCustom.exe |
| Aggiungere il riavvio automatico dopo un crash | ZoomItCustomSupervisor.exe |

Il **supervisore** apre ZoomIt Custom e resta in background. Basta avviare lui, senza aprire separatamente l'app. Se l'app è già aperta da sola, chiuderla prima di avviare il supervisore. Uscire da ZoomIt Custom dal menu dell'icona nell'area di notifica chiude entrambi.

Dopo un crash, il supervisore riavvia l'app e tenta di ripristinare l'ultimo stato valido: modalità, ingrandimento, colore del disegno e annotazioni completate. Il disegno ritorna sospeso; un clic sinistro permette di continuare. Il tratto in corso e la cronologia degli annullamenti non vengono recuperati. Se il ripristino non è possibile, l'app torna al desktop. Un secondo crash entro un minuto provoca un riavvio sul desktop; al terzo si fermano i riavvii automatici.

Avviando direttamente l'app, le protezioni interne restano attive, ma dopo un crash occorre riaprirla manualmente.

## Avvio con Windows

Aprire le opzioni dall'icona nell'area di notifica. In **Avvio automatico (all'accesso a Windows)** scegliere:

- **Disattivato**: avvio manuale.
- **Avvia ZoomIt Custom con Windows**: avvio dell'app senza supervisore.
- **Avvia ZoomIt Custom con supervisore**: avvio del supervisore, che apre e supervisiona l'app.

Confermare con **OK**. La scelta si applica al prossimo accesso a Windows e non cambia la sessione corrente. Le opzioni indicano se la sessione in corso è supervisionata. La scelta con supervisore non è disponibile se il suo eseguibile manca dalla cartella dell'app.

Se si sposta la cartella, riconfermare la scelta per aggiornare il percorso di avvio. Se Windows ha disabilitato l'app nelle impostazioni delle app di avvio, riabilitarla anche lì.

## Uso quotidiano

Le scorciatoie si possono modificare nelle opzioni. LiveDraw usa sempre la scorciatoia Draw configurata, con l'aggiunta di Shift; la combinazione attuale è mostrata nella scheda Draw. Scegliere una scorciatoia Draw senza Shift per mantenere distinte le due azioni.

| Scorciatoia iniziale | Azione |
| --- | --- |
| Ctrl+1 | Zoom statico |
| Ctrl+2 | LiveZoom |
| Ctrl+3 | Disegno |
| Ctrl+4 | Pausa con timer |
| Ctrl+5 | Copia un ritaglio |
| Ctrl+Shift+3 | LiveDraw (scorciatoia Draw + Shift) |
| Ctrl+Shift+5 | Salva un ritaglio PNG |

Lo Zoom statico blocca l'immagine ingrandita. LiveZoom la aggiorna in tempo reale; LiveDraw permette di disegnare su questa visualizzazione dal vivo.

Ogni modalità ha la propria checkbox **Animate zoom in and zoom out**. In LiveZoom, attivandola, ingresso, uscita e variazioni di ingrandimento diventano graduali; disattivandola sono immediati. L'opzione LiveZoom parte disattivata e viene salvata indipendentemente da quella dello Zoom.

In LiveZoom, **Ctrl+Su / Ctrl+Giù** regolano l'ingrandimento. **Ctrl+3** avvia Draw su un'immagine bloccata; **Ctrl+Shift+3** avvia LiveDraw mantenendo la visualizzazione dal vivo. **Esc** termina il disegno e torna a LiveZoom. **Ctrl+2** esce da LiveZoom solo quando il disegno non è attivo.

Le scorciatoie di modalità seguono queste regole, anche durante le animazioni dello zoom:

- Dal desktop si può avviare qualsiasi modalità. Nello Zoom statico, le scorciatoie Draw, LiveDraw, LiveZoom e timer sono ignorate; la scorciatoia Zoom esce dallo Zoom.
- In LiveZoom senza disegno sono disponibili Draw, LiveDraw e Snip; Zoom e timer sono ignorati.
- Con Draw o LiveDraw attivi o sospesi, le scorciatoie Zoom, LiveZoom, timer e delle modalità di disegno non cambiano nulla. Per terminare il disegno usare i comandi del mouse o Esc.
- Nel timer è accettata soltanto la sua scorciatoia di modalità; ripremerla riavvia il conto alla rovescia.
- Durante selezione Snip, salvataggio o opzioni, le scorciatoie di modalità sono ignorate. I comandi ignorati non vengono accodati per eseguirli in seguito.

In Draw e LiveDraw, **Ctrl+Z** annulla ed **E** cancella le annotazioni. Il primo **clic destro** sospende il disegno e mostra un piccolo cerchio con bordo del colore attivo e interno scuro. Spostare il mouse non aggiunge tratti. Il **clic sinistro** riprende il disegno; un secondo **clic destro** esce. Anche **Esc** termina il disegno. LiveDraw non supporta evidenziatore e sfocatura. Nel timer, Su/Giù e rotella regolano i minuti; Sinistra/Destra regolano dieci secondi ed **Esc** termina la pausa.

Snip funziona sul desktop, nello Zoom statico, in LiveZoom e in Draw, anche con disegno sospeso. Dopo cattura o annullamento conserva annotazioni, ingrandimento e stato del disegno. Con LiveZoom blocca temporaneamente l'immagine e poi ripristina la visualizzazione dal vivo. Snip è ignorato durante un tratto in corso, nel timer o con LiveDraw su LiveZoom; resta disponibile con LiveDraw sul desktop. **Ctrl+Shift+5** salva un PNG seguendo le stesse regole. Nel salvataggio, scegliere **Zoomed PNG** per la dimensione visualizzata oppure **Actual size PNG** per eliminare l'ingrandimento.

## In caso di problemi

I rapporti vengono creati **solo per crash o errori gravi**, mai durante l'uso normale. Per trovarli, incollare questo percorso in Esplora file o nella finestra Esegui di Windows:

    %LOCALAPPDATA%\ZoomItCustom\Logs

Se la cartella non è scrivibile, viene usata **%TEMP%\ZoomItCustom-Logs**. Il rapporto di testo descrive l'operazione e l'errore; può essere accompagnato da un file di diagnostica.

## Requisiti e limiti

- Windows 10 o Windows 11 aggiornati, a 64 bit. Non serve installare separatamente il runtime C++. Gli eseguibili non sono firmati digitalmente.
- Il consumo di memoria cresce con la risoluzione dello schermo. La cronologia degli annullamenti è limitata per contenerlo. Il recupero supervisionato opzionale può usare fino a 128 MiB per le immagini salvate; le annotazioni non sono recuperabili da immagini più grandi di 64 MiB ciascuna.
- Il recupero riguarda i crash dell'app, non un'app che resta aperta ma smette di rispondere, né il riavvio o lo spegnimento di Windows.
- Il timer sul secondo schermo richiede il desktop esteso. Con schermi duplicati resta sul monitor corrente. Un cambio di schermo o il ritorno dalla sospensione può terminare la modalità attiva; riattivarla per continuare.
- LiveZoom non è disponibile su Windows Server 2022 e sulle build di Windows 11 21H2 precedenti alla revisione 829. Usare una versione aggiornata di Windows.
- Il funzionamento con monitor a DPI differenti, Desktop remoto, touch e penna non è garantito.

## Compilazione dai sorgenti

Installare Visual Studio 2022 o successivo con strumenti C++, Windows SDK e CMake. Dalla cartella dei sorgenti eseguire:

    .\build.ps1

I due eseguibili vengono creati in **build-Release**.

## Copyright e licenza

Basato sui [sorgenti ZoomIt di Microsoft PowerToys](https://github.com/microsoft/PowerToys/tree/21fd5092b3e062ca6c8dd6b8c772a236f90b3b42/src/modules/ZoomIt/ZoomIt).

Modifiche Custom: copyright © 2026 Prof. ing. Raffaele Mele. Codice originale: Mark Russinovich / Microsoft Corporation. Distribuito con [licenza MIT](LICENSE); conservare la licenza e gli avvisi di copyright nelle redistribuzioni.
