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

Le scorciatoie si possono modificare nelle opzioni.

| Scorciatoia iniziale | Azione |
| --- | --- |
| Ctrl+1 | Zoom statico |
| Ctrl+2 | LiveZoom |
| Ctrl+3 | Disegno |
| Ctrl+4 | Pausa con timer |
| Ctrl+5 | Copia un ritaglio |
| Ctrl+Shift+2 | LiveDraw |
| Ctrl+Shift+5 | Salva un ritaglio PNG |

Lo Zoom statico blocca l'immagine ingrandita. LiveZoom la aggiorna in tempo reale; LiveDraw permette di disegnare su questa visualizzazione dal vivo.

In LiveZoom, **Ctrl+Su / Ctrl+Giù** regolano l'ingrandimento. **Ctrl+1 viene ignorato** mentre LiveZoom è attivo. **Ctrl+3** passa al disegno su un'immagine bloccata; **Esc** torna a LiveZoom. **Ctrl+2** chiude LiveZoom e l'eventuale LiveDraw attivo.

Nel disegno, **Ctrl+Z** annulla ed **E** cancella le annotazioni. Il primo **clic destro** sospende il disegno e mostra un piccolo cerchio con bordo del colore attivo e interno scuro. Spostare il mouse non aggiunge tratti. Il **clic sinistro** riprende il disegno; un secondo **clic destro** esce da Draw. Anche **Esc** termina la modalità. Nel timer, le frecce regolano la durata ed **Esc** termina la pausa.

Snip funziona anche nello Zoom statico, comprese le annotazioni. Con LiveZoom blocca temporaneamente l'immagine e poi ripristina la visualizzazione dal vivo. Snip non è disponibile mentre LiveDraw è attivo. Nel salvataggio, scegliere **Zoomed PNG** per la dimensione visualizzata oppure **Actual size PNG** per eliminare l'ingrandimento.

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
