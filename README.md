# ZoomIt Custom

Applicazione portatile per Windows per ingrandire lo schermo, disegnare e mostrare un timer. Include LiveZoom, LiveDraw e ritagli PNG. Registrazione video/audio, Type e DemoType sono state rimosse.

## Installazione

Scaricare [il pacchetto portatile o l’eseguibile dalla pagina delle release](https://github.com/lellomele/ZoomItCustom/releases/latest). Estrarre il pacchetto portatile e avviare ZoomItCustom.exe. Chiudere eventuali altre istanze di ZoomIt che utilizzano le stesse scorciatoie.

Le impostazioni sono conservate in HKEY_CURRENT_USER\Software\ZoomItCustom\ZoomIt Custom; l'avvio automatico usa la voce ZoomIt Custom. Al primo avvio dopo l’aggiornamento dalla precedente mappatura, le cinque scorciatoie vengono impostate come indicato sotto; le successive personalizzazioni vengono conservate.

## Uso

| Scorciatoia iniziale | Funzione |
| --- | --- |
| Ctrl+1 | Zoom statico |
| Ctrl+2 | LiveZoom |
| Ctrl+3 | Disegno |
| Ctrl+4 | Pausa con timer |
| Ctrl+5 | Snip: copia un ritaglio |
| Ctrl+Shift+2 | LiveDraw |
| Ctrl+Shift+5 | Salva un ritaglio PNG |

In LiveZoom, Ctrl+Su e Ctrl+Giù regolano l'ingrandimento. Il comando Zoom (inizialmente Ctrl+1) viene ignorato mentre LiveZoom è attivo. Ctrl+3 passa al disegno statico; Esc torna a LiveZoom. Ctrl+2 chiude LiveZoom e l'eventuale LiveDraw attivo. La chiusura di LiveZoom è immediata.

Nel disegno, Ctrl+Z annulla ed E cancella le annotazioni. Il primo clic destro sospende il disegno: il pointer diventa un piccolo cerchio con bordo del colore attivo e interno scuro. Spostare il mouse non aggiunge tratti. Il clic sinistro ripristina il pointer precedente e permette di continuare dal punto indicato; il secondo clic destro esce dalla modalità Draw. Esc termina la modalità. Nel timer, le frecce regolano la durata ed Esc termina la pausa. Le scorciatoie e le altre opzioni si configurano dal menu dell'icona nell'area di notifica.

Snip funziona anche nello zoom statico già attivo e ne mantiene la visualizzazione dopo il ritaglio o l'annullamento. Cattura ciò che appare ingrandito sullo schermo, comprese le annotazioni. Nel salvataggio, scegliere Zoomed PNG per mantenere la dimensione visibile oppure Actual size PNG per ridurla secondo il fattore di zoom. Partendo da LiveZoom, il ritaglio blocca temporaneamente l'immagine e poi ripristina LiveZoom. Snip è disabilitato quando LiveZoom e LiveDraw sono attivi contemporaneamente.

## Requisiti e limiti

Windows 10 o Windows 11 aggiornati, a 64 bit. L'eseguibile non è firmato digitalmente. Non occorre installare il runtime C++.

La cronologia del disegno conserva fino a 32 immagini, con un budget indicativo di 64 MiB: fino a 8 immagini Full HD o 2 immagini 4K. Conserva sempre almeno un annullamento, anche se una singola immagine supera il budget. Le immagini necessarie al disegno e allo zoom richiedono ulteriore memoria, proporzionale alla risoluzione.

Il percorso LiveZoom per Windows Server 2022 e Windows 11 21H2 prima della revisione 829 richiede privilegi UIAccess. In questa distribuzione portatile tali privilegi non sono disponibili: LiveZoom viene disabilitato su queste build e le opzioni mostrano un avviso. Usare una versione aggiornata di Windows 10/11. Il requisito è documentato da [Microsoft](https://learn.microsoft.com/en-us/windows/win32/api/magnification/nf-magnification-magsetinputtransform).

Il corretto funzionamento con più monitor a DPI differenti, desktop remoto, touch e penna non è garantito.

## Compilazione

Installare Visual Studio 2022 o successivo con gli strumenti C++, Windows SDK e CMake per Windows. Dalla cartella dei sorgenti:

    .\build.ps1

Il risultato è build-Release\ZoomItCustom.exe. Per la versione Debug usare:

    .\build.ps1 -Configuration Debug

Per eseguire anche le prove sul desktop:

    .\build.ps1 -RunTests

Queste prove mostrano temporaneamente zoom e finestre dell'applicazione.

La scheda About, dopo Snip, riporta versione, autore, origine e licenza. Il numero di versione compare nella barra del titolo delle opzioni. L'icona è integrata nell'eseguibile e nelle finestre. I sorgenti grafici e le dimensioni di distribuzione sono in assets; generate-icon.ps1 rigenera PNG e ICO.

## Origine e copyright

Derivato dai sorgenti Microsoft PowerToys che integrano ZoomIt 9.0, commit [21fd5092b3e062ca6c8dd6b8c772a236f90b3b42](https://github.com/microsoft/PowerToys/tree/21fd5092b3e062ca6c8dd6b8c772a236f90b3b42/src/modules/ZoomIt/ZoomIt). Questa versione non è una ricompilazione dei sorgenti originali di ZoomIt 6.12.

Modifiche della versione Custom: copyright (C) 2026 Prof. ing. Raffaele Mele. Codice originale: copyright Mark Russinovich / Microsoft Corporation. Licenza MIT: vedere LICENSE. Per redistribuire i sorgenti o l'eseguibile derivato, conservare la licenza e gli avvisi di copyright. L'eseguibile originale Sysinternals 6.12 non è incluso nei pacchetti.
