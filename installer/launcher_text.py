# tbc launcher text in German, French, Spanish and Italian, keyed by the English (the code's own).
#
# Translated by AI (Claude) and not yet checked by native speakers: the launcher says so beside its
# language selector. The source is the "Launcher Text" sheet (installer text, all 281 strings of
# 0.3.0), plus the strings added since. A string missing here shows in English, so a new English
# string never breaks a translated launcher; add its four translations here when there's time.
#
# Placeholders in {braces} stay as they are: tr() fills them in after translating.

import ctypes
import re

LANGUAGE_CODES = ("en", "de", "fr", "es", "it")
NATIVE_NAMES = {"en": "English", "de": "Deutsch", "fr": "Français", "es": "Español", "it": "Italiano"}
# The game's language setting (the launcher's Game files tab) -> a launcher language.
GAME_LANGUAGE_CODE = {"English": "en", "German": "de", "French": "fr", "Spanish": "es", "Italian": "it"}
_COLUMN = {"de": 0, "fr": 1, "es": 2, "it": 3}
_language = "en"


def set_language(code):
    global _language
    _language = code if code in LANGUAGE_CODES else "en"


def language():
    return _language


def system_language():
    """Windows' display language, if the launcher has it; else English."""
    try:
        primary = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF
    except (AttributeError, OSError):
        return "en"
    return {0x07: "de", 0x0C: "fr", 0x0A: "es", 0x10: "it"}.get(primary, "en")


def resolve(choice, game_language=None):
    """The launcher language for the setting: a code, or "same" (as the game; Windows' language
    when there's no game setting yet)."""
    if choice in LANGUAGE_CODES:
        return choice
    if game_language in GAME_LANGUAGE_CODE:
        return GAME_LANGUAGE_CODE[game_language]
    return system_language()


def tr(text, context=None, **values):
    """The text in the launcher's language, with {placeholders} filled in from values."""
    if _language != "en":
        row = CONTEXT.get((context, text)) or TEXT.get(text)
        if row:
            text = row[_COLUMN[_language]]
    return text.format(**values) if values else text


_PARTS = re.compile(r"^(.*?)( \([^()]*\))$")


def tr_label(label):
    """A setting's choice: the whole label if it's known, else its words in brackets at the end
    (render resolutions are built from numbers and such a part: "1680×720 (original height)")."""
    if _language == "en" or label in TEXT:
        return tr(label)
    m = _PARTS.match(label)
    if m and m.group(2) in TEXT:
        return m.group(1) + tr(m.group(2))
    return label


# The same English with another meaning: (context, English).
CONTEXT = {
    ("column", "Save"): ("Spielstand", "Sauvegarde", "Partida", "Salvataggio"),
}

TEXT = {
    'The Bourne Conspiracy': (
        'The Bourne Conspiracy',
        'The Bourne Conspiracy',
        'The Bourne Conspiracy',
        'The Bourne Conspiracy'),
    'THE BOURNE CONSPIRACY': (
        'THE BOURNE CONSPIRACY',
        'THE BOURNE CONSPIRACY',
        'THE BOURNE CONSPIRACY',
        'THE BOURNE CONSPIRACY'),
    'Launcher {version}': (
        'Launcher {version}',
        'Lanceur {version}',
        'Lanzador {version}',
        'Launcher {version}'),
    "Your game was built with {built}; this launcher is {version}. Reinstall to rebuild the game with this version's changes.": (
        'Dein Spiel wurde mit {built} erstellt, dieser Launcher ist {version}. Installiere neu, um das Spiel mit den Änderungen dieser Version neu zu erstellen.',
        'Votre jeu a été compilé avec la version {built} ; ce lanceur est la version {version}. Réinstallez pour recompiler le jeu avec les changements de cette version.',
        'Tu juego se compiló con la versión {built}; este lanzador es la {version}. Reinstala para recompilar el juego con los cambios de esta versión.',
        'Il gioco è stato compilato con la versione {built}; questo launcher è la {version}. Reinstalla per ricompilare il gioco con le modifiche di questa versione.'),
    'Reinstall now': (
        'Jetzt neu installieren',
        'Réinstaller maintenant',
        'Reinstalar ahora',
        'Reinstalla ora'),
    'Update available: {version} (you have {current}).': (
        'Update verfügbar: {version} (du hast {current}).',
        'Mise à jour disponible : {version} (vous avez la {current}).',
        'Actualización disponible: {version} (tienes la {current}).',
        'Aggiornamento disponibile: {version} (hai la {current}).'),
    'Download and update': (
        'Herunterladen und aktualisieren',
        'Télécharger et mettre à jour',
        'Descargar y actualizar',
        'Scarica e aggiorna'),
    "What's new": (
        'Was ist neu',
        'Nouveautés',
        'Novedades',
        'Novità'),
    'Game files': (
        'Spieldateien',
        'Fichiers du jeu',
        'Archivos del juego',
        'File di gioco'),
    'Display': (
        'Anzeige',
        'Affichage',
        'Pantalla',
        'Schermo'),
    'Graphics': (
        'Grafik',
        'Graphismes',
        'Gráficos',
        'Grafica'),
    'Camera': (
        'Kamera',
        'Caméra',
        'Cámara',
        'Telecamera'),
    'Controls': (
        'Steuerung',
        'Commandes',
        'Controles',
        'Comandi'),
    'Saves': (
        'Spielstände',
        'Sauvegardes',
        'Partidas guardadas',
        'Salvataggi'),
    'Achievements': (
        'Erfolge',
        'Succès',
        'Logros',
        'Obiettivi'),
    'Installed in {folder}': (
        'Installiert in {folder}',
        'Installé dans {folder}',
        'Instalado en {folder}',
        'Installato in {folder}'),
    'Reinstall from a disc image...': (
        'Von einem Disc-Abbild neu installieren...',
        'Réinstaller depuis une image disque...',
        'Reinstalar desde una imagen de disco...',
        "Reinstalla da un'immagine disco..."),
    'Use a different install...': (
        'Andere Installation verwenden...',
        'Utiliser une autre installation...',
        'Usar otra instalación...',
        "Usa un'altra installazione..."),
    'Help': (
        'Hilfe',
        'Aide',
        'Ayuda',
        'Aiuto'),
    'Open logs': (
        'Logs öffnen',
        'Ouvrir les journaux',
        'Abrir registros',
        'Apri i log'),
    'Save crash report': (
        'Absturzbericht speichern',
        'Enregistrer un rapport de plantage',
        'Guardar informe de error',
        'Salva rapporto di arresto anomalo'),
    'Check for updates': (
        'Nach Updates suchen',
        'Rechercher des mises à jour',
        'Buscar actualizaciones',
        'Cerca aggiornamenti'),
    'Game': (
        'Spiel',
        'Jeu',
        'Juego',
        'Gioco'),
    'Language': (
        'Sprache',
        'Langue',
        'Idioma',
        'Lingua'),
    'English': (
        'Englisch',
        'Anglais',
        'Inglés',
        'Inglese'),
    'German': (
        'Deutsch',
        'Allemand',
        'Alemán',
        'Tedesco'),
    'French': (
        'Französisch',
        'Français',
        'Francés',
        'Francese'),
    'Spanish': (
        'Spanisch',
        'Espagnol',
        'Español',
        'Spagnolo'),
    'Italian': (
        'Italienisch',
        'Italien',
        'Italiano',
        'Italiano'),
    "The game's voices, subtitles and text, all in this language (each language has only its own voices and subtitles). The achievement names follow it too.": (
        'Stimmen, Untertitel und Texte des Spiels, alle in dieser Sprache (jede Sprache hat nur ihre eigenen Stimmen und Untertitel). Auch die Namen der Erfolge folgen ihr.',
        "Les voix, sous-titres et textes du jeu, tous dans cette langue (chaque langue n'a que ses propres voix et sous-titres). Les noms des succès la suivent aussi.",
        'Las voces, subtítulos y textos del juego, todo en este idioma (cada idioma solo tiene sus propias voces y subtítulos). Los nombres de los logros también lo siguen.',
        'Voci, sottotitoli e testi del gioco, tutti in questa lingua (ogni lingua ha solo le proprie voci e i propri sottotitoli). Anche i nomi degli obiettivi la seguono.'),
    'Renderer': (
        'Renderer',
        'Moteur de rendu',
        'Renderizador',
        'Renderer'),
    'Native (Vulkan, recommended)': (
        'Nativ (Vulkan, empfohlen)',
        'Natif (Vulkan, recommandé)',
        'Nativo (Vulkan, recomendado)',
        'Nativo (Vulkan, consigliato)'),
    'Xenos (Direct3D 12)': (
        'Xenos (Direct3D 12)',
        'Xenos (Direct3D 12)',
        'Xenos (Direct3D 12)',
        'Xenos (Direct3D 12)'),
    'Fullscreen': (
        'Vollbild',
        'Plein écran',
        'Pantalla completa',
        'Schermo intero'),
    'Output resolution': (
        'Ausgabeauflösung',
        'Résolution de sortie',
        'Resolución de salida',
        'Risoluzione di uscita'),
    'Original (720p)': (
        'Original (720p)',
        'Originale (720p)',
        'Original (720p)',
        'Originale (720p)'),
    '1080p': (
        '1080p',
        '1080p',
        '1080p',
        '1080p'),
    '1440p': (
        '1440p',
        '1440p',
        '1440p',
        '1440p'),
    '4K': (
        '4K',
        '4K',
        '4K',
        '4K'),
    '2560×1080 ultrawide (experimental)': (
        '2560×1080 Ultrawide (experimentell)',
        '2560×1080 ultra-large (expérimental)',
        '2560×1080 ultrapanorámico (experimental)',
        '2560×1080 ultrawide (sperimentale)'),
    '3440×1440 ultrawide (experimental)': (
        '3440×1440 Ultrawide (experimentell)',
        '3440×1440 ultra-large (expérimental)',
        '3440×1440 ultrapanorámico (experimental)',
        '3440×1440 ultrawide (sperimentale)'),
    '3840×1600 ultrawide (experimental)': (
        '3840×1600 Ultrawide (experimentell)',
        '3840×1600 ultra-large (expérimental)',
        '3840×1600 ultrapanorámico (experimental)',
        '3840×1600 ultrawide (sperimentale)'),
    'Ultrawide outputs are experimental and need the native renderer.': (
        'Ultrawide-Ausgaben sind experimentell und benötigen den nativen Renderer.',
        'Les sorties ultra-larges sont expérimentales et nécessitent le moteur de rendu natif.',
        'Las salidas ultrapanorámicas son experimentales y necesitan el renderizador nativo.',
        'Le uscite ultrawide sono sperimentali e richiedono il renderer nativo.'),
    'Render resolution (sharper, slower)': (
        'Renderauflösung (schärfer, langsamer)',
        'Résolution de rendu (plus nette, plus lente)',
        'Resolución de renderizado (más nítida, más lenta)',
        'Risoluzione di rendering (più nitida, più lenta)'),
    '1280×720 (original)': (
        '1280×720 (original)',
        '1280×720 (originale)',
        '1280×720 (original)',
        '1280×720 (originale)'),
    '1920×1080 (Xenos: 1440p)': (
        '1920×1080 (Xenos: 1440p)',
        '1920×1080 (Xenos : 1440p)',
        '1920×1080 (Xenos: 1440p)',
        '1920×1080 (Xenos: 1440p)'),
    '2560×1440 (demanding)': (
        '2560×1440 (anspruchsvoll)',
        '2560×1440 (exigeant)',
        '2560×1440 (exigente)',
        '2560×1440 (impegnativo)'),
    '3840×2160 (very demanding)': (
        '3840×2160 (sehr anspruchsvoll)',
        '3840×2160 (très exigeant)',
        '3840×2160 (muy exigente)',
        '3840×2160 (molto impegnativo)'),
    ' (original height)': (
        ' (Originalhöhe)',
        " (hauteur d'origine)",
        ' (altura original)',
        ' (altezza originale)'),
    ' (your screen)': (
        ' (dein Bildschirm)',
        ' (votre écran)',
        ' (tu pantalla)',
        ' (il tuo schermo)'),
    ' (very demanding)': (
        ' (sehr anspruchsvoll)',
        ' (très exigeant)',
        ' (muy exigente)',
        ' (molto impegnativo)'),
    'Graphics path': (
        'Grafikpfad',
        'Chemin graphique',
        'Ruta gráfica',
        'Percorso grafico'),
    'Auto (ROV above 720p)': (
        'Automatisch (ROV über 720p)',
        'Auto (ROV au-dessus de 720p)',
        'Automático (ROV por encima de 720p)',
        'Automatico (ROV sopra 720p)'),
    'Render targets (RTV)': (
        'Render-Targets (RTV)',
        'Cibles de rendu (RTV)',
        'Destinos de renderizado (RTV)',
        'Render target (RTV)'),
    'Shader EDRAM (ROV)': (
        'Shader-EDRAM (ROV)',
        'EDRAM par shader (ROV)',
        'EDRAM por shader (ROV)',
        'EDRAM via shader (ROV)'),
    'Upscaling filter': (
        'Skalierungsfilter',
        "Filtre de mise à l'échelle",
        'Filtro de escalado',
        'Filtro di upscaling'),
    'Bilinear (original)': (
        'Bilinear (original)',
        "Bilinéaire (d'origine)",
        'Bilineal (original)',
        'Bilineare (originale)'),
    'FSR (sharper 3D, softer text)': (
        'FSR (schärferes 3D, weicherer Text)',
        'FSR (3D plus net, texte plus doux)',
        'FSR (3D más nítido, texto más suave)',
        'FSR (3D più nitido, testo più morbido)'),
    'CAS (sharpen only)': (
        'CAS (nur Schärfen)',
        'CAS (netteté uniquement)',
        'CAS (solo nitidez)',
        'CAS (solo nitidezza)'),
    'Anti-aliasing': (
        'Kantenglättung',
        'Anticrénelage',
        'Antialiasing',
        'Antialiasing'),
    'Off': (
        'Aus',
        'Désactivé',
        'Desactivado',
        'Disattivato'),
    'FXAA': (
        'FXAA',
        'FXAA',
        'FXAA',
        'FXAA'),
    'FXAA (strong)': (
        'FXAA (stark)',
        'FXAA (fort)',
        'FXAA (fuerte)',
        'FXAA (forte)'),
    'Frame rate': (
        'Bildrate',
        "Fréquence d'images",
        'Velocidad de fotogramas',
        'Frequenza fotogrammi'),
    '30 (original)': (
        '30 (original)',
        "30 (d'origine)",
        '30 (original)',
        '30 (originale)'),
    '120 (experimental)': (
        '120 (experimentell)',
        '120 (expérimental)',
        '120 (experimental)',
        '120 (sperimentale)'),
    '144 (experimental)': (
        '144 (experimentell)',
        '144 (expérimental)',
        '144 (experimental)',
        '144 (sperimentale)'),
    'Unlimited (experimental)': (
        'Unbegrenzt (experimentell)',
        'Illimitée (expérimental)',
        'Ilimitada (experimental)',
        'Illimitata (sperimentale)'),
    'FPS counter': (
        'FPS-Anzeige',
        "Compteur d'IPS",
        'Contador de FPS',
        'Contatore FPS'),
    'FPS': (
        'FPS',
        'IPS',
        'FPS',
        'FPS'),
    'Detailed (GPU, CPU, memory)': (
        'Detailliert (GPU, CPU, Speicher)',
        'Détaillé (GPU, CPU, mémoire)',
        'Detallado (GPU, CPU, memoria)',
        'Dettagliato (GPU, CPU, memoria)'),
    'VSync (off: may tear, can be faster)': (
        'VSync (aus: kann reißen, kann schneller sein)',
        'VSync (désactivée : déchirures possibles, peut être plus rapide)',
        'VSync (desactivada: puede haber tearing, puede ir más rápido)',
        'VSync (disattivata: possibile tearing, può essere più veloce)'),
    'Original Xbox 360 frame pacing (slower)': (
        'Original-Xbox-360-Bildtakt (langsamer)',
        "Cadence d'images de la Xbox 360 d'origine (plus lente)",
        'Ritmo de fotogramas original de Xbox 360 (más lento)',
        'Cadenza fotogrammi originale Xbox 360 (più lenta)'),
    'Hide achievement pop-ups': (
        'Erfolgs-Pop-ups ausblenden',
        'Masquer les notifications de succès',
        'Ocultar avisos de logros',
        'Nascondi i popup degli obiettivi'),
    'Native renderer by the Skullburglar project (github.com/nlwstein/skullburglar, BSD 3-Clause).': (
        'Nativer Renderer vom Skullburglar-Projekt (github.com/nlwstein/skullburglar, BSD 3-Clause).',
        'Moteur de rendu natif par le projet Skullburglar (github.com/nlwstein/skullburglar, BSD 3-Clause).',
        'Renderizador nativo del proyecto Skullburglar (github.com/nlwstein/skullburglar, BSD 3-Clause).',
        'Renderer nativo del progetto Skullburglar (github.com/nlwstein/skullburglar, BSD 3-Clause).'),
    'Motion blur': (
        'Bewegungsunschärfe',
        'Flou de mouvement',
        'Desenfoque de movimiento',
        'Sfocatura di movimento'),
    'Depth of field': (
        'Tiefenschärfe',
        'Profondeur de champ',
        'Profundidad de campo',
        'Profondità di campo'),
    'Bloom': (
        'Bloom',
        'Bloom',
        'Bloom',
        'Bloom'),
    'Lens flares': (
        'Linsenreflexe',
        "Reflets d'objectif",
        'Destellos de lente',
        'Riflessi lente'),
    'Heat haze / distortion': (
        'Hitzeflimmern / Verzerrung',
        'Brume de chaleur / distorsion',
        'Calima / distorsión',
        'Foschia di calore / distorsione'),
    'Brightness (%)': (
        'Helligkeit (%)',
        'Luminosité (%)',
        'Brillo (%)',
        'Luminosità (%)'),
    'Brightness is for the native renderer (Xenos ignores it); black and white stay as they are.': (
        'Helligkeit gilt für den nativen Renderer (Xenos ignoriert sie); Schwarz und Weiß bleiben unverändert.',
        "La luminosité s'applique au moteur de rendu natif (Xenos l'ignore) ; le noir et le blanc restent inchangés.",
        'El brillo es para el renderizador nativo (Xenos lo ignora); el negro y el blanco no cambian.',
        'La luminosità vale per il renderer nativo (Xenos la ignora); nero e bianco restano invariati.'),
    'Field of view (%)': (
        'Sichtfeld (%)',
        'Champ de vision (%)',
        'Campo de visión (%)',
        'Campo visivo (%)'),
    'Look further up and down': (
        'Weiter nach oben und unten schauen',
        'Regarder plus loin en haut et en bas',
        'Mirar más arriba y abajo',
        'Guarda più in alto e in basso'),
    'No camera auto-recentre': (
        'Keine automatische Kamera-Zentrierung',
        'Pas de recentrage automatique de la caméra',
        'Sin recentrado automático de la cámara',
        'Nessun ricentramento automatico della telecamera'),
    'Linear stick turning (no turn acceleration, controller)': (
        'Lineares Stick-Drehen (keine Dreh-Beschleunigung, Controller)',
        'Rotation linéaire au stick (sans accélération, manette)',
        'Giro lineal con el stick (sin aceleración de giro, mando)',
        'Rotazione lineare con lo stick (senza accelerazione, controller)'),
    'Keyboard and mouse': (
        'Tastatur und Maus',
        'Clavier et souris',
        'Teclado y ratón',
        'Tastiera e mouse'),
    'Key binds': (
        'Tastenbelegung',
        'Touches',
        'Asignación de teclas',
        'Tasti'),
    'Stick dead zone (%)': (
        'Stick-Totzone (%)',
        'Zone morte du stick (%)',
        'Zona muerta del stick (%)',
        'Zona morta dello stick (%)'),
    'Stick turn speed (%)': (
        'Stick-Drehgeschwindigkeit (%)',
        'Vitesse de rotation du stick (%)',
        'Velocidad de giro del stick (%)',
        'Velocità di rotazione dello stick (%)'),
    'Controller vibration (%, 0 = off)': (
        'Controller-Vibration (%, 0 = aus)',
        'Vibrations de la manette (%, 0 = désactivées)',
        'Vibración del mando (%, 0 = desactivada)',
        'Vibrazione del controller (%, 0 = disattivata)'),
    'Mouse': (
        'Maus',
        'Souris',
        'Ratón',
        'Mouse'),
    'Mouse turns the camera directly (mouse look)': (
        'Maus dreht die Kamera direkt (Maussteuerung)',
        'La souris tourne directement la caméra (visée souris)',
        'El ratón gira la cámara directamente (vista con ratón)',
        'Il mouse ruota direttamente la telecamera (mouse look)'),
    'Direct (1:1)': (
        'Direkt (1:1)',
        'Direct (1:1)',
        'Directo (1:1)',
        'Diretto (1:1)'),
    'Mouse sensitivity (%)': (
        'Mausempfindlichkeit (%)',
        'Sensibilité de la souris (%)',
        'Sensibilidad del ratón (%)',
        'Sensibilità del mouse (%)'),
    'Aiming sensitivity (% of mouse)': (
        'Zielempfindlichkeit (% der Maus)',
        'Sensibilité de visée (% de la souris)',
        'Sensibilidad al apuntar (% del ratón)',
        'Sensibilità di mira (% del mouse)'),
    'Mouse camera smoothing (%)': (
        'Kameraglättung der Maus (%)',
        'Lissage de la caméra à la souris (%)',
        'Suavizado de cámara con ratón (%)',
        'Smussamento della telecamera col mouse (%)'),
    "Smoothing: how much of the game's camera easing stays with mouse look (lower is more responsive). Aiming sensitivity applies over the shoulder and when shooting from cover.": (
        'Glättung: wie viel der Kameraverzögerung des Spiels bei Maussteuerung bleibt (weniger reagiert direkter). Die Zielempfindlichkeit gilt beim Zielen über die Schulter und beim Schießen aus der Deckung.',
        "Lissage : la part de l'amortissement de caméra du jeu conservée avec la souris (moins = plus réactif). La sensibilité de visée s'applique en visée par-dessus l'épaule et en tirant depuis un abri.",
        'Suavizado: cuánto del suavizado de cámara del juego se mantiene con el ratón (menos es más directo). La sensibilidad al apuntar se aplica al apuntar sobre el hombro y al disparar desde cobertura.',
        "Smussamento: quanta parte dell'ammorbidimento della telecamera del gioco resta con il mouse (meno è più reattivo). La sensibilità di mira vale mirando sopra la spalla e sparando dalla copertura."),
    'Open saves folder': (
        'Spielstand-Ordner öffnen',
        'Ouvrir le dossier des sauvegardes',
        'Abrir carpeta de partidas',
        'Apri la cartella dei salvataggi'),
    'Back up saves': (
        'Spielstände sichern',
        'Sauvegarder les parties',
        'Copia de seguridad de partidas',
        'Esegui backup dei salvataggi'),
    'Refresh': (
        'Aktualisieren',
        'Actualiser',
        'Actualizar',
        'Aggiorna'),
    'Save': (
        'Speichern',
        'Enregistrer',
        'Guardar',
        'Salva'),
    'Last saved': (
        'Zuletzt gespeichert',
        'Dernier enregistrement',
        'Guardada por última vez',
        'Ultimo salvataggio'),
    'Size': (
        'Größe',
        'Taille',
        'Tamaño',
        'Dimensione'),
    'Folder': (
        'Ordner',
        'Dossier',
        'Carpeta',
        'Cartella'),
    '{n} KB': (
        '{n} KB',
        '{n} Ko',
        '{n} KB',
        '{n} KB'),
    '{n} saved game. ': (
        '{n} Spielstand. ',
        '{n} sauvegarde. ',
        '{n} partida guardada. ',
        '{n} salvataggio. '),
    '{n} saved games. ': (
        '{n} Spielstände. ',
        '{n} sauvegardes. ',
        '{n} partidas guardadas. ',
        '{n} salvataggi. '),
    'No saved games yet. ': (
        'Noch keine Spielstände. ',
        "Aucune sauvegarde pour l'instant. ",
        'Aún no hay partidas guardadas. ',
        'Ancora nessun salvataggio. '),
    'Saves folder: {folder}': (
        'Spielstand-Ordner: {folder}',
        'Dossier des sauvegardes : {folder}',
        'Carpeta de partidas: {folder}',
        'Cartella dei salvataggi: {folder}'),
    'Double-click a save to open its folder.': (
        'Doppelklicke auf einen Spielstand, um seinen Ordner zu öffnen.',
        'Double-cliquez sur une sauvegarde pour ouvrir son dossier.',
        'Haz doble clic en una partida para abrir su carpeta.',
        'Fai doppio clic su un salvataggio per aprirne la cartella.'),
    '{got} of {total} unlocked    {score} of {max} G': (
        '{got} von {total} freigeschaltet    {score} von {max} G',
        '{got} sur {total} débloqués    {score} sur {max} G',
        '{got} de {total} desbloqueados    {score} de {max} G',
        '{got} di {total} sbloccati    {score} di {max} G'),
    'Unlocked {date}': (
        'Freigeschaltet am {date}',
        'Débloqué le {date}',
        'Desbloqueado el {date}',
        'Sbloccato il {date}'),
    'Locked': (
        'Gesperrt',
        'Verrouillé',
        'Bloqueado',
        'Bloccato'),
    'Start the game once (to the title screen) and its achievements appear here.': (
        'Starte das Spiel einmal (bis zum Titelbildschirm), dann erscheinen hier seine Erfolge.',
        "Lancez le jeu une fois (jusqu'à l'écran titre) et ses succès apparaîtront ici.",
        'Inicia el juego una vez (hasta la pantalla de título) y sus logros aparecerán aquí.',
        'Avvia il gioco una volta (fino alla schermata del titolo) e i suoi obiettivi appariranno qui.'),
    'Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec': (
        'Jan. Feb. März Apr. Mai Juni Juli Aug. Sep. Okt. Nov. Dez.',
        'janv. févr. mars avr. mai juin juil. août sept. oct. nov. déc.',
        'ene. feb. mar. abr. may. jun. jul. ago. sept. oct. nov. dic.',
        'gen feb mar apr mag giu lug ago set ott nov dic'),
    '{n} G': (
        '{n} G',
        '{n} G',
        '{n} G',
        '{n} G'),
    "Couldn't read the achievements: {error}": (
        'Die Erfolge konnten nicht gelesen werden: {error}',
        'Impossible de lire les succès : {error}',
        'No se pudieron leer los logros: {error}',
        'Impossibile leggere gli obiettivi: {error}'),
    'Play': (
        'Spielen',
        'Jouer',
        'Jugar',
        'Gioca'),
    'Quit': (
        'Beenden',
        'Quitter',
        'Salir',
        'Esci'),
    'Defaults': (
        'Standard',
        'Par défaut',
        'Predeterminado',
        'Predefiniti'),
    'Close launcher when the game starts': (
        'Launcher beim Spielstart schließen',
        'Fermer le lanceur au lancement du jeu',
        'Cerrar el lanzador al iniciar el juego',
        "Chiudi il launcher all'avvio del gioco"),
    'Experimental options may cause glitches. Set them back to default to restore the original game.': (
        'Experimentelle Optionen können Fehler verursachen. Setze sie auf Standard zurück, um das Originalspiel wiederherzustellen.',
        "Les options expérimentales peuvent causer des problèmes. Remettez-les par défaut pour retrouver le jeu d'origine.",
        'Las opciones experimentales pueden causar fallos. Devuélvelas a sus valores predeterminados para recuperar el juego original.',
        'Le opzioni sperimentali possono causare problemi. Riportale ai valori predefiniti per ripristinare il gioco originale.'),
    'Settings saved.': (
        'Einstellungen gespeichert.',
        'Paramètres enregistrés.',
        'Ajustes guardados.',
        'Impostazioni salvate.'),
    'Started with {summary}.': (
        'Gestartet mit {summary}.',
        'Lancé avec {summary}.',
        'Iniciado con {summary}.',
        'Avviato con {summary}.'),
    'original game config': (
        'Original-Spielkonfiguration',
        "configuration d'origine du jeu",
        'configuración original del juego',
        'configurazione originale del gioco'),
    '{n} config change(s)': (
        '{n} Konfigurationsänderung(en)',
        '{n} modification(s) de configuration',
        '{n} cambio(s) de configuración',
        '{n} modifica/e alla configurazione'),
    'Checking for updates...': (
        'Suche nach Updates...',
        'Recherche de mises à jour...',
        'Buscando actualizaciones...',
        'Ricerca aggiornamenti...'),
    "Couldn't reach GitHub to check for updates.": (
        'GitHub war nicht erreichbar, um nach Updates zu suchen.',
        'Impossible de joindre GitHub pour rechercher des mises à jour.',
        'No se pudo conectar con GitHub para buscar actualizaciones.',
        'Impossibile raggiungere GitHub per cercare aggiornamenti.'),
    'Version {version} is out: "Download and update" above.': (
        'Version {version} ist erschienen: oben "Herunterladen und aktualisieren".',
        'La version {version} est disponible : « Télécharger et mettre à jour » ci-dessus.',
        'Ya está la versión {version}: "Descargar y actualizar" arriba.',
        'È uscita la versione {version}: "Scarica e aggiorna" qui sopra.'),
    'You have the latest version ({version}).': (
        'Du hast die neueste Version ({version}).',
        'Vous avez la dernière version ({version}).',
        'Tienes la versión más reciente ({version}).',
        "Hai l'ultima versione ({version})."),
    'Downloading {version}...': (
        'Lade {version} herunter...',
        'Téléchargement de la {version}...',
        'Descargando {version}...',
        'Download di {version}...'),
    'Download failed ({error}). Use "What\'s new" to download it yourself.': (
        'Download fehlgeschlagen ({error}). Lade es über "Was ist neu" selbst herunter.',
        'Échec du téléchargement ({error}). Utilisez « Nouveautés » pour le télécharger vous-même.',
        'La descarga falló ({error}). Usa "Novedades" para descargarlo tú mismo.',
        'Download non riuscito ({error}). Usa "Novità" per scaricarlo tu.'),
    'Downloaded {version} to {folder}.': (
        '{version} nach {folder} heruntergeladen.',
        '{version} téléchargée dans {folder}.',
        '{version} descargada en {folder}.',
        '{version} scaricata in {folder}.'),
    'On foot': (
        'Zu Fuß',
        'À pied',
        'A pie',
        'A piedi'),
    'Move': (
        'Bewegen',
        'Se déplacer',
        'Moverse',
        'Muoversi'),
    'Look around': (
        'Umsehen',
        'Regarder autour',
        'Mirar alrededor',
        'Guardarsi intorno'),
    'Left mouse': (
        'Linke Maustaste',
        'Clic gauche',
        'Botón izquierdo',
        'Tasto sinistro'),
    'Fire': (
        'Schießen',
        'Tirer',
        'Disparar',
        'Spara'),
    'Right mouse': (
        'Rechte Maustaste',
        'Clic droit',
        'Botón derecho',
        'Tasto destro'),
    'Aim; in cover with a gun, pop out': (
        'Zielen; in Deckung mit Waffe: hervorkommen',
        'Viser ; à couvert avec une arme, sortir',
        'Apuntar; a cubierto con un arma, asomarse',
        "Mira; in copertura con un'arma, sporgiti"),
    'Shift': (
        'Umschalt',
        'Maj',
        'Mayús',
        'Maiusc'),
    'Sprint': (
        'Sprinten',
        'Sprinter',
        'Esprintar',
        'Scatto'),
    'Ctrl': (
        'Strg',
        'Ctrl',
        'Ctrl',
        'Ctrl'),
    'Crouch': (
        'Ducken',
        "S'accroupir",
        'Agacharse',
        'Accovacciarsi'),
    'Space or Enter': (
        'Leertaste oder Eingabe',
        'Espace ou Entrée',
        'Espacio o Intro',
        'Spazio o Invio'),
    'Action: doors, climb, vault, cover; block': (
        'Aktion: Türen, klettern, springen, Deckung; blocken',
        'Action : portes, escalader, sauter, se mettre à couvert ; bloquer',
        'Acción: puertas, trepar, saltar, cobertura; bloquear',
        'Azione: porte, arrampicarsi, scavalcare, copertura; parata'),
    'F or Backspace': (
        'F oder Rücktaste',
        'F ou Retour arrière',
        'F o Retroceso',
        'F o Backspace'),
    'Takedown; skip cutscenes': (
        'Takedown; Zwischensequenzen überspringen',
        'Neutralisation ; passer les cinématiques',
        'Derribo; saltar escenas',
        'Atterramento; salta i filmati'),
    'E or R': (
        'E oder R',
        'E ou R',
        'E o R',
        'E o R'),
    'Reload': (
        'Nachladen',
        'Recharger',
        'Recargar',
        'Ricarica'),
    'Bourne Instinct': (
        'Bourne-Instinkt',
        'Instinct de Bourne',
        'Instinto Bourne',
        'Istinto di Bourne'),
    'Draw / holster the gun': (
        'Waffe ziehen / wegstecken',
        "Dégainer / ranger l'arme",
        'Sacar / guardar el arma',
        "Estrai / riponi l'arma"),
    'Middle mouse': (
        'Mittlere Maustaste',
        'Clic du milieu',
        'Botón central',
        'Tasto centrale'),
    'Recentre the camera; switch shoulder when aiming': (
        'Kamera zentrieren; beim Zielen Schulter wechseln',
        "Recentrer la caméra ; changer d'épaule en visant",
        'Recentrar la cámara; cambiar de hombro al apuntar',
        'Ricentra la telecamera; cambia spalla mirando'),
    'D-pad up / down / left / right': (
        'Steuerkreuz hoch / runter / links / rechts',
        'Croix directionnelle haut / bas / gauche / droite',
        'Cruceta arriba / abajo / izquierda / derecha',
        'Croce direzionale su / giù / sinistra / destra'),
    'Esc': (
        'Esc',
        'Échap',
        'Esc',
        'Esc'),
    'Pause': (
        'Pause',
        'Pause',
        'Pausa',
        'Pausa'),
    'Tab': (
        'Tab',
        'Tab',
        'Tab',
        'Tab'),
    'Back': (
        'Zurück',
        'Retour',
        'Atrás',
        'Indietro'),
    'Fights': (
        'Kämpfe',
        'Combats',
        'Peleas',
        'Combattimenti'),
    'Light attack': (
        'Leichter Angriff',
        'Attaque légère',
        'Ataque ligero',
        'Attacco leggero'),
    'Heavy attack': (
        'Schwerer Angriff',
        'Attaque lourde',
        'Ataque fuerte',
        'Attacco pesante'),
    'Space': (
        'Leertaste',
        'Espace',
        'Espacio',
        'Spazio'),
    'Block': (
        'Blocken',
        'Bloquer',
        'Bloquear',
        'Parata'),
    'Takedown, once a tier of the adrenaline meter is full': (
        'Takedown, sobald eine Stufe der Adrenalinanzeige voll ist',
        "Neutralisation, dès qu'un niveau de la jauge d'adrénaline est plein",
        'Derribo, cuando un nivel del medidor de adrenalina está lleno',
        "Atterramento, quando un livello dell'indicatore di adrenalina è pieno"),
    "Quick-time events: the button's place on the diamond": (
        'Quick-Time-Events: Position der Taste auf der Raute',
        'Actions contextuelles : position du bouton sur le losange',
        'Eventos rápidos: posición del botón en el rombo',
        'Quick time event: posizione del tasto sul rombo'),
    'Top (Y)': (
        'Oben (Y)',
        'Haut (Y)',
        'Arriba (Y)',
        'Sopra (Y)'),
    'Left (X)': (
        'Links (X)',
        'Gauche (X)',
        'Izquierda (X)',
        'Sinistra (X)'),
    'Bottom (A)': (
        'Unten (A)',
        'Bas (A)',
        'Abajo (A)',
        'Sotto (A)'),
    'Right (B)': (
        'Rechts (B)',
        'Droite (B)',
        'Derecha (B)',
        'Destra (B)'),
    'Menus': (
        'Menüs',
        'Menus',
        'Menús',
        'Menu'),
    'Enter or Space': (
        'Eingabe oder Leertaste',
        'Entrée ou Espace',
        'Intro o Espacio',
        'Invio o Spazio'),
    'Accept; start on the title screen': (
        'Bestätigen; auf dem Titelbildschirm starten',
        "Valider ; démarrer sur l'écran titre",
        'Aceptar; empezar en la pantalla de título',
        'Conferma; avvia nella schermata del titolo'),
    'Backspace or F': (
        'Rücktaste oder F',
        'Retour arrière ou F',
        'Retroceso o F',
        'Backspace o F'),
    'W / S or arrow keys': (
        'W / S oder Pfeiltasten',
        'W / S ou flèches',
        'W / S o flechas',
        'W / S o frecce'),
    'Move up and down': (
        'Hoch und runter bewegen',
        'Monter et descendre',
        'Mover arriba y abajo',
        'Muovi su e giù'),
    'The layout is fixed: the prompts and the key images in the game are drawn for it.': (
        'Die Belegung ist fest: Die Hinweise und Tastenbilder im Spiel sind dafür gezeichnet.',
        'La disposition est fixe : les indications et les images de touches du jeu sont dessinées pour elle.',
        'La distribución es fija: los avisos y las imágenes de teclas del juego están hechos para ella.',
        'La disposizione è fissa: i suggerimenti e le immagini dei tasti nel gioco sono disegnati per essa.'),
    'Close': (
        'Schließen',
        'Fermer',
        'Cerrar',
        'Chiudi'),
    'Full page with notes': (
        'Ganze Seite mit Hinweisen',
        'Page complète avec notes',
        'Página completa con notas',
        'Pagina completa con note'),
    'Update': (
        'Update',
        'Mise à jour',
        'Actualización',
        'Aggiornamento'),
    'Crash report': (
        'Absturzbericht',
        'Rapport de plantage',
        'Informe de error',
        'Rapporto di arresto anomalo'),
    'Locate': (
        'Suchen',
        'Localiser',
        'Localizar',
        'Individua'),
    'Install running': (
        'Installation läuft',
        'Installation en cours',
        'Instalación en curso',
        'Installazione in corso'),
    '{version} is downloaded.\n\nStart it now? This launcher closes, and the new one asks you to reinstall the game (pick your disc image) so the game is rebuilt with the update.': (
        '{version} ist heruntergeladen.\n\nJetzt starten? Dieser Launcher wird geschlossen, und der neue bittet dich, das Spiel neu zu installieren (wähle dein Disc-Abbild), damit es mit dem Update neu erstellt wird.',
        'La {version} est téléchargée.\n\nLa lancer maintenant ? Ce lanceur se ferme, et le nouveau vous demande de réinstaller le jeu (choisissez votre image disque) pour le recompiler avec la mise à jour.',
        'La {version} está descargada.\n\n¿Iniciarla ahora? Este lanzador se cierra y el nuevo te pedirá reinstalar el juego (elige tu imagen de disco) para recompilarlo con la actualización.',
        "La {version} è stata scaricata.\n\nAvviarla ora? Questo launcher si chiude e il nuovo ti chiederà di reinstallare il gioco (scegli la tua immagine disco) per ricompilarlo con l'aggiornamento."),
    "Couldn't start {file}: {error}": (
        '{file} konnte nicht gestartet werden: {error}',
        'Impossible de lancer {file} : {error}',
        'No se pudo iniciar {file}: {error}',
        'Impossibile avviare {file}: {error}'),
    "Couldn't open {folder}: {error}": (
        '{folder} konnte nicht geöffnet werden: {error}',
        "Impossible d'ouvrir {folder} : {error}",
        'No se pudo abrir {folder}: {error}',
        'Impossibile aprire {folder}: {error}'),
    "Couldn't back up the saves: {error}": (
        'Die Spielstände konnten nicht gesichert werden: {error}',
        'Impossible de sauvegarder les parties : {error}',
        'No se pudo hacer la copia de seguridad de las partidas: {error}',
        'Impossibile eseguire il backup dei salvataggi: {error}'),
    'Saved {file} to {folder}.\n\nTo restore it, close the game and unzip it into {saves folder}, replacing the files there.': (
        '{file} wurde in {folder} gespeichert.\n\nZum Wiederherstellen schließe das Spiel und entpacke die Datei nach {saves folder}, wobei die vorhandenen Dateien ersetzt werden.',
        '{file} enregistré dans {folder}.\n\nPour le restaurer, fermez le jeu et décompressez-le dans {saves folder} en remplaçant les fichiers existants.',
        '{file} guardado en {folder}.\n\nPara restaurarlo, cierra el juego y descomprímelo en {saves folder}, reemplazando los archivos que haya.',
        '{file} salvato in {folder}.\n\nPer ripristinarlo, chiudi il gioco ed estrailo in {saves folder}, sostituendo i file presenti.'),
    "Couldn't open the folder: {error}": (
        'Der Ordner konnte nicht geöffnet werden: {error}',
        "Impossible d'ouvrir le dossier : {error}",
        'No se pudo abrir la carpeta: {error}',
        'Impossibile aprire la cartella: {error}'),
    "Couldn't save the report: {error}": (
        'Der Bericht konnte nicht gespeichert werden: {error}',
        "Impossible d'enregistrer le rapport : {error}",
        'No se pudo guardar el informe: {error}',
        'Impossibile salvare il rapporto: {error}'),
    'Saved {file} to {folder}.\n\nAttach it when you report a problem. It contains the game log, your launcher settings and any crash dump (no game files).': (
        '{file} wurde in {folder} gespeichert.\n\nHänge die Datei an, wenn du ein Problem meldest. Sie enthält das Spiel-Log, deine Launcher-Einstellungen und ggf. einen Absturz-Dump (keine Spieldateien).',
        '{file} enregistré dans {folder}.\n\nJoignez-le quand vous signalez un problème. Il contient le journal du jeu, vos paramètres du lanceur et un éventuel vidage de plantage (aucun fichier du jeu).',
        '{file} guardado en {folder}.\n\nAdjúntalo cuando informes de un problema. Contiene el registro del juego, tus ajustes del lanzador y cualquier volcado de error (ningún archivo del juego).',
        '{file} salvato in {folder}.\n\nAllegalo quando segnali un problema. Contiene il log del gioco, le impostazioni del launcher ed eventuali dump di arresto anomalo (nessun file del gioco).'),
    'Save your changed settings before quitting?': (
        'Geänderte Einstellungen vor dem Beenden speichern?',
        'Enregistrer les paramètres modifiés avant de quitter ?',
        '¿Guardar los ajustes cambiados antes de salir?',
        'Salvare le impostazioni modificate prima di uscire?'),
    "The game hasn't been rebuilt for {version} yet, so this version's changes aren't in it, and its new settings may not work.\n\nPlay anyway? (Reinstall now to rebuild it.)": (
        'Das Spiel wurde noch nicht für {version} neu erstellt, daher fehlen die Änderungen dieser Version und neue Einstellungen funktionieren eventuell nicht.\n\nTrotzdem spielen? (Installiere jetzt neu, um es neu zu erstellen.)',
        "Le jeu n'a pas encore été recompilé pour la {version} : les changements de cette version n'y sont pas, et ses nouveaux paramètres risquent de ne pas fonctionner.\n\nJouer quand même ? (Réinstallez maintenant pour le recompiler.)",
        'El juego aún no se ha recompilado para la {version}, así que los cambios de esta versión no están y sus nuevos ajustes pueden no funcionar.\n\n¿Jugar de todos modos? (Reinstala ahora para recompilarlo.)',
        'Il gioco non è ancora stato ricompilato per la {version}, quindi le modifiche di questa versione mancano e le nuove impostazioni potrebbero non funzionare.\n\nGiocare comunque? (Reinstalla ora per ricompilarlo.)'),
    "Couldn't update the game's config files. Is the game already running?": (
        'Die Konfigurationsdateien des Spiels konnten nicht aktualisiert werden. Läuft das Spiel bereits?',
        'Impossible de mettre à jour les fichiers de configuration du jeu. Le jeu est-il déjà lancé ?',
        'No se pudieron actualizar los archivos de configuración del juego. ¿Ya está abierto el juego?',
        'Impossibile aggiornare i file di configurazione del gioco. Il gioco è già in esecuzione?'),
    "Couldn't apply the config settings ({error}).\n\nStart the game with its original config instead?": (
        'Die Konfigurationseinstellungen konnten nicht angewendet werden ({error}).\n\nDas Spiel stattdessen mit seiner Originalkonfiguration starten?',
        "Impossible d'appliquer les paramètres de configuration ({error}).\n\nLancer le jeu avec sa configuration d'origine à la place ?",
        'No se pudieron aplicar los ajustes de configuración ({error}).\n\n¿Iniciar el juego con su configuración original?',
        'Impossibile applicare le impostazioni di configurazione ({error}).\n\nAvviare il gioco con la configurazione originale?'),
    "Couldn't change the intro movies ({error}). Starting anyway.": (
        'Die Intro-Videos konnten nicht geändert werden ({error}). Starte trotzdem.',
        "Impossible de modifier les vidéos d'introduction ({error}). Lancement quand même.",
        'No se pudieron cambiar los vídeos de introducción ({error}). Se inicia de todos modos.',
        'Impossibile modificare i video introduttivi ({error}). Avvio comunque.'),
    "The native renderer isn't installed (this build's SDK wasn't made with it). Starting with Xenos instead.": (
        'Der native Renderer ist nicht installiert (das SDK dieses Builds enthält ihn nicht). Starte stattdessen mit Xenos.',
        "Le moteur de rendu natif n'est pas installé (le SDK de cette version ne l'inclut pas). Lancement avec Xenos à la place.",
        'El renderizador nativo no está instalado (el SDK de esta compilación no lo incluye). Se inicia con Xenos.',
        "Il renderer nativo non è installato (l'SDK di questa build non lo include). Avvio con Xenos."),
    "Couldn't start the game: {error}": (
        'Das Spiel konnte nicht gestartet werden: {error}',
        'Impossible de lancer le jeu : {error}',
        'No se pudo iniciar el juego: {error}',
        'Impossibile avviare il gioco: {error}'),
    "That folder doesn't contain default.xex.": (
        'Dieser Ordner enthält keine default.xex.',
        'Ce dossier ne contient pas default.xex.',
        'Esa carpeta no contiene default.xex.',
        'Quella cartella non contiene default.xex.'),
    'An install is still running. Quit anyway?\nRunning the launcher again will pick up where it left off.': (
        'Eine Installation läuft noch. Trotzdem beenden?\nWenn du den Launcher erneut startest, macht er dort weiter, wo er aufgehört hat.',
        "Une installation est encore en cours. Quitter quand même ?\nRelancer le lanceur reprendra là où il s'est arrêté.",
        'Todavía hay una instalación en curso. ¿Salir de todos modos?\nAl volver a abrir el lanzador, continuará donde se quedó.',
        "C'è ancora un'installazione in corso. Uscire comunque?\nRiaprendo il launcher, riprenderà da dove si era fermato."),
    'Already installed? Locate tbc.exe...': (
        'Bereits installiert? tbc.exe suchen...',
        'Déjà installé ? Localiser tbc.exe...',
        '¿Ya instalado? Localizar tbc.exe...',
        'Già installato? Individua tbc.exe...'),
    'Locate tbc.exe': (
        'tbc.exe suchen',
        'Localiser tbc.exe',
        'Localizar tbc.exe',
        'Individua tbc.exe'),
    'Programs': (
        'Programme',
        'Programmes',
        'Programas',
        'Programmi'),
    'Choose the extracted game folder (the one with default.xex)': (
        'Wähle den entpackten Spielordner (den mit default.xex)',
        'Choisissez le dossier du jeu extrait (celui avec default.xex)',
        'Elige la carpeta del juego extraída (la que tiene default.xex)',
        'Scegli la cartella del gioco estratta (quella con default.xex)'),
    'The Bourne Conspiracy - Install & Play': (
        'The Bourne Conspiracy - Installieren & Spielen',
        'The Bourne Conspiracy - Installer et jouer',
        'The Bourne Conspiracy - Instalar y jugar',
        'The Bourne Conspiracy - Installa e gioca'),
    'Your disc. One installer. Ready to play.': (
        'Deine Disc. Ein Installer. Bereit zum Spielen.',
        'Votre disque. Un installateur. Prêt à jouer.',
        'Tu disco. Un instalador. Listo para jugar.',
        'Il tuo disco. Un installer. Pronto per giocare.'),
    '1  Choose your game disc image': (
        '1  Wähle dein Disc-Abbild des Spiels',
        "1  Choisissez l'image disque du jeu",
        '1  Elige la imagen de disco del juego',
        "1  Scegli l'immagine disco del gioco"),
    'Choose ISO...': (
        'ISO wählen...',
        "Choisir l'ISO...",
        'Elegir ISO...',
        'Scegli ISO...'),
    '2  Install location': (
        '2  Installationsort',
        "2  Emplacement d'installation",
        '2  Ubicación de instalación',
        '2  Percorso di installazione'),
    'Change...': (
        'Ändern...',
        'Modifier...',
        'Cambiar...',
        'Cambia...'),
    'The installer reads your ISO and builds the game on this PC. First setup may download several GB and take a while. Windows may ask for administrator permission.': (
        'Der Installer liest deine ISO und erstellt das Spiel auf diesem PC. Die erste Einrichtung lädt eventuell mehrere GB herunter und dauert eine Weile. Windows fragt eventuell nach Administratorrechten.',
        "L'installateur lit votre ISO et compile le jeu sur ce PC. La première installation peut télécharger plusieurs Go et prendre un moment. Windows peut demander l'autorisation d'administrateur.",
        'El instalador lee tu ISO y compila el juego en este PC. La primera instalación puede descargar varios GB y tardar un rato. Windows puede pedir permiso de administrador.',
        "L'installer legge la tua ISO e compila il gioco su questo PC. La prima installazione può scaricare diversi GB e richiedere un po' di tempo. Windows potrebbe chiedere l'autorizzazione di amministratore."),
    'Allow required components to be downloaded and installed (Visual Studio Build Tools, ReXGlue SDK)': (
        'Herunterladen und Installieren benötigter Komponenten erlauben (Visual Studio Build Tools, ReXGlue SDK)',
        "Autoriser le téléchargement et l'installation des composants nécessaires (Visual Studio Build Tools, ReXGlue SDK)",
        'Permitir descargar e instalar los componentes necesarios (Visual Studio Build Tools, ReXGlue SDK)',
        "Consenti il download e l'installazione dei componenti necessari (Visual Studio Build Tools, ReXGlue SDK)"),
    'Choose your ISO, then click Install.': (
        'Wähle deine ISO und klicke dann auf Installieren.',
        'Choisissez votre ISO, puis cliquez sur Installer.',
        'Elige tu ISO y luego haz clic en Instalar.',
        'Scegli la tua ISO, poi fai clic su Installa.'),
    '+ Installation details': (
        '+ Installationsdetails',
        "+ Détails de l'installation",
        '+ Detalles de la instalación',
        "+ Dettagli dell'installazione"),
    '- Installation details': (
        '- Installationsdetails',
        "- Détails de l'installation",
        '- Detalles de la instalación',
        "- Dettagli dell'installazione"),
    'Install': (
        'Installieren',
        'Installer',
        'Instalar',
        'Installa'),
    'Installing...': (
        'Installiere...',
        'Installation...',
        'Instalando...',
        'Installazione...'),
    'Continue': (
        'Weiter',
        'Continuer',
        'Continuar',
        'Continua'),
    'Open log': (
        'Log öffnen',
        'Ouvrir le journal',
        'Abrir registro',
        'Apri il log'),
    'Choose your game disc image': (
        'Wähle dein Disc-Abbild des Spiels',
        "Choisissez l'image disque du jeu",
        'Elige la imagen de disco del juego',
        "Scegli l'immagine disco del gioco"),
    'Disc images': (
        'Disc-Abbilder',
        'Images disque',
        'Imágenes de disco',
        'Immagini disco'),
    'All files': (
        'Alle Dateien',
        'Tous les fichiers',
        'Todos los archivos',
        'Tutti i file'),
    'Choose install location': (
        'Installationsort wählen',
        "Choisir l'emplacement d'installation",
        'Elegir ubicación de instalación',
        'Scegli il percorso di installazione'),
    'Log': (
        'Log',
        'Journal',
        'Registro',
        'Log'),
    "There's no install log yet.": (
        'Es gibt noch kein Installations-Log.',
        "Il n'y a pas encore de journal d'installation.",
        'Todavía no hay registro de instalación.',
        "Non c'è ancora un log di installazione."),
    'Choose ISO': (
        'ISO wählen',
        "Choisir l'ISO",
        'Elegir ISO',
        'Scegli ISO'),
    "Choose your game's .iso file first.": (
        'Wähle zuerst die .iso-Datei deines Spiels.',
        "Choisissez d'abord le fichier .iso de votre jeu.",
        'Elige primero el archivo .iso de tu juego.',
        'Scegli prima il file .iso del gioco.'),
    'Install location': (
        'Installationsort',
        "Emplacement d'installation",
        'Ubicación de instalación',
        'Percorso di installazione'),
    'Please choose an install location without these characters: {characters}\n\nThe build tools treat them as special characters and the build fails.\nFor example: {folder}': (
        'Bitte wähle einen Installationsort ohne diese Zeichen: {characters}\n\nDie Build-Tools behandeln sie als Sonderzeichen und der Build schlägt fehl.\nZum Beispiel: {folder}',
        "Choisissez un emplacement d'installation sans ces caractères : {characters}\n\nLes outils de compilation les traitent comme des caractères spéciaux et la compilation échoue.\nPar exemple : {folder}",
        'Elige una ubicación de instalación sin estos caracteres: {characters}\n\nLas herramientas de compilación los tratan como caracteres especiales y la compilación falla.\nPor ejemplo: {folder}',
        'Scegli un percorso di installazione senza questi caratteri: {characters}\n\nGli strumenti di compilazione li trattano come caratteri speciali e la compilazione non riesce.\nAd esempio: {folder}'),
    'non-English letters': (
        'nicht-englische Buchstaben',
        'lettres non anglaises',
        'letras que no son del inglés',
        'lettere non inglesi'),
    'Installed! A desktop shortcut was created.': (
        'Installiert! Eine Desktop-Verknüpfung wurde erstellt.',
        'Installé ! Un raccourci a été créé sur le bureau.',
        '¡Instalado! Se ha creado un acceso directo en el escritorio.',
        'Installato! È stato creato un collegamento sul desktop.'),
    'Install stopped. See the message, or click "Open log".': (
        'Installation abgebrochen. Siehe die Meldung oder klicke auf "Log öffnen".',
        'Installation arrêtée. Lisez le message ou cliquez sur « Ouvrir le journal ».',
        'Instalación detenida. Lee el mensaje o haz clic en "Abrir registro".',
        'Installazione interrotta. Leggi il messaggio o fai clic su "Apri il log".'),
    'Install stopped': (
        'Installation abgebrochen',
        'Installation arrêtée',
        'Instalación detenida',
        'Installazione interrotta'),
    'Checking game files...': (
        'Prüfe Spieldateien...',
        'Vérification des fichiers du jeu...',
        'Comprobando los archivos del juego...',
        'Verifica dei file di gioco...'),
    'Reading your disc image...': (
        'Lese dein Disc-Abbild...',
        'Lecture de votre image disque...',
        'Leyendo tu imagen de disco...',
        "Lettura dell'immagine disco..."),
    'Extracting game files...': (
        'Entpacke Spieldateien...',
        'Extraction des fichiers du jeu...',
        'Extrayendo los archivos del juego...',
        'Estrazione dei file di gioco...'),
    'Extracting game files... {percent}%': (
        'Entpacke Spieldateien... {percent} %',
        'Extraction des fichiers du jeu... {percent} %',
        'Extrayendo los archivos del juego... {percent} %',
        'Estrazione dei file di gioco... {percent}%'),
    'Looking for C++ build tools...': (
        'Suche C++-Build-Tools...',
        'Recherche des outils de compilation C++...',
        'Buscando las herramientas de compilación de C++...',
        'Ricerca degli strumenti di compilazione C++...'),
    'Downloading Visual Studio Build Tools installer...': (
        'Lade den Installer der Visual Studio Build Tools herunter...',
        "Téléchargement du programme d'installation de Visual Studio Build Tools...",
        'Descargando el instalador de Visual Studio Build Tools...',
        'Download del programma di installazione di Visual Studio Build Tools...'),
    'Installing Visual Studio Build Tools (several GB, this can take 10-30 minutes)...': (
        'Installiere Visual Studio Build Tools (mehrere GB, kann 10-30 Minuten dauern)...',
        'Installation de Visual Studio Build Tools (plusieurs Go, cela peut prendre 10 à 30 minutes)...',
        'Instalando Visual Studio Build Tools (varios GB, puede tardar de 10 a 30 minutos)...',
        'Installazione di Visual Studio Build Tools (diversi GB, può richiedere 10-30 minuti)...'),
    'Unpacking the ReXGlue SDK...': (
        'Entpacke das ReXGlue SDK...',
        'Décompression du SDK ReXGlue...',
        'Descomprimiendo el SDK de ReXGlue...',
        "Estrazione dell'SDK ReXGlue..."),
    'Downloading ReXGlue SDK {version}...': (
        'Lade ReXGlue SDK {version} herunter...',
        'Téléchargement du SDK ReXGlue {version}...',
        'Descargando el SDK de ReXGlue {version}...',
        "Download dell'SDK ReXGlue {version}..."),
    'Preparing project files...': (
        'Bereite Projektdateien vor...',
        'Préparation des fichiers du projet...',
        'Preparando los archivos del proyecto...',
        'Preparazione dei file del progetto...'),
    "Recompiling the game code (a few minutes, the bar won't move much)...": (
        'Kompiliere den Spielcode neu (einige Minuten, der Balken bewegt sich kaum)...',
        'Recompilation du code du jeu (quelques minutes, la barre bougera peu)...',
        'Recompilando el código del juego (unos minutos, la barra apenas se moverá)...',
        'Ricompilazione del codice del gioco (qualche minuto, la barra si muoverà poco)...'),
    'Configuring the build...': (
        'Konfiguriere den Build...',
        'Configuration de la compilation...',
        'Configurando la compilación...',
        'Configurazione della compilazione...'),
    'Building the game (compiling: this takes a while)...': (
        'Erstelle das Spiel (Kompilieren, das dauert eine Weile)...',
        'Compilation du jeu (cela prend un moment)...',
        'Compilando el juego (esto tarda un rato)...',
        "Compilazione del gioco (richiede un po' di tempo)..."),
    'Building the game... {percent}%': (
        'Erstelle das Spiel... {percent} %',
        'Compilation du jeu... {percent} %',
        'Compilando el juego... {percent} %',
        'Compilazione del gioco... {percent}%'),
    "This doesn't look like an Xbox 360 disc image.\nChoose the .iso file you made from your game disc.": (
        'Das sieht nicht nach einem Xbox-360-Disc-Abbild aus.\nWähle die .iso-Datei, die du von deiner Spiel-Disc erstellt hast.',
        'Cela ne ressemble pas à une image disque Xbox 360.\nChoisissez le fichier .iso créé à partir de votre disque de jeu.',
        'Esto no parece una imagen de disco de Xbox 360.\nElige el archivo .iso que creaste a partir del disco del juego.',
        "Questa non sembra un'immagine disco Xbox 360.\nScegli il file .iso creato dal disco del gioco."),
    'The disc image ends early while reading {file}.\nIt may be incomplete or damaged.': (
        'Das Disc-Abbild endet vorzeitig beim Lesen von {file}.\nEs ist eventuell unvollständig oder beschädigt.',
        "L'image disque se termine prématurément lors de la lecture de {file}.\nElle est peut-être incomplète ou endommagée.",
        'La imagen de disco termina antes de tiempo al leer {file}.\nPuede estar incompleta o dañada.',
        "L'immagine disco termina prima del previsto durante la lettura di {file}.\nPotrebbe essere incompleta o danneggiata."),
    "default.xex on this disc isn't a valid Xbox 360 executable.": (
        'default.xex auf dieser Disc ist keine gültige Xbox-360-Programmdatei.',
        "default.xex sur ce disque n'est pas un exécutable Xbox 360 valide.",
        'default.xex en este disco no es un ejecutable de Xbox 360 válido.',
        'default.xex su questo disco non è un eseguibile Xbox 360 valido.'),
    "Couldn't read the title ID from default.xex.": (
        'Die Titel-ID konnte nicht aus default.xex gelesen werden.',
        "Impossible de lire l'ID du titre dans default.xex.",
        'No se pudo leer el ID del título de default.xex.',
        "Impossibile leggere l'ID del titolo da default.xex."),
    "Couldn't find the ReXGlue SDK {version} for Windows on GitHub.": (
        'ReXGlue SDK {version} für Windows wurde auf GitHub nicht gefunden.',
        'Impossible de trouver le SDK ReXGlue {version} pour Windows sur GitHub.',
        'No se encontró el SDK de ReXGlue {version} para Windows en GitHub.',
        "Impossibile trovare l'SDK ReXGlue {version} per Windows su GitHub."),
    'A build step failed. Click "Open log" for details.\n\nLast output:\n{output}': (
        'Ein Build-Schritt ist fehlgeschlagen. Klicke auf "Log öffnen" für Details.\n\nLetzte Ausgabe:\n{output}',
        'Une étape de compilation a échoué. Cliquez sur « Ouvrir le journal » pour les détails.\n\nDernière sortie :\n{output}',
        'Falló un paso de la compilación. Haz clic en "Abrir registro" para ver los detalles.\n\nÚltima salida:\n{output}',
        'Un passaggio della compilazione non è riuscito. Fai clic su "Apri il log" per i dettagli.\n\nUltimo output:\n{output}'),
    'Not enough free space. The install needs about {needed} GB, and the chosen drive has {free} GB free.': (
        'Nicht genug freier Speicher. Die Installation braucht etwa {needed} GB, auf dem gewählten Laufwerk sind {free} GB frei.',
        "Espace libre insuffisant. L'installation nécessite environ {needed} Go, et le lecteur choisi a {free} Go libres.",
        'No hay suficiente espacio libre. La instalación necesita unos {needed} GB y la unidad elegida tiene {free} GB libres.',
        "Spazio libero insufficiente. L'installazione richiede circa {needed} GB e l'unità scelta ha {free} GB liberi."),
    "Some game files couldn't be extracted from the disc image:\n{files}\nThe image may be incomplete or damaged.": (
        'Einige Spieldateien konnten nicht aus dem Disc-Abbild entpackt werden:\n{files}\nDas Abbild ist eventuell unvollständig oder beschädigt.',
        "Certains fichiers du jeu n'ont pas pu être extraits de l'image disque :\n{files}\nL'image est peut-être incomplète ou endommagée.",
        'No se pudieron extraer algunos archivos del juego de la imagen de disco:\n{files}\nLa imagen puede estar incompleta o dañada.',
        "Alcuni file di gioco non sono stati estratti dall'immagine disco:\n{files}\nL'immagine potrebbe essere incompleta o danneggiata."),
    'This disc image has no default.xex. Is it an Xbox 360 game disc?': (
        'Dieses Disc-Abbild enthält keine default.xex. Ist es eine Xbox-360-Spiel-Disc?',
        "Cette image disque ne contient pas default.xex. S'agit-il d'un disque de jeu Xbox 360 ?",
        'Esta imagen de disco no tiene default.xex. ¿Es un disco de juego de Xbox 360?',
        'Questa immagine disco non contiene default.xex. È un disco di gioco Xbox 360?'),
    'This disc image is a different game (title ID {id}).\nThis installer needs The Bourne Conspiracy (title ID 565507D4).': (
        'Dieses Disc-Abbild ist ein anderes Spiel (Titel-ID {id}).\nDieser Installer braucht The Bourne Conspiracy (Titel-ID 565507D4).',
        'Cette image disque est un autre jeu (ID du titre {id}).\nCet installateur nécessite The Bourne Conspiracy (ID du titre 565507D4).',
        'Esta imagen de disco es de otro juego (ID del título {id}).\nEste instalador necesita The Bourne Conspiracy (ID del título 565507D4).',
        'Questa immagine disco è di un altro gioco (ID titolo {id}).\nQuesto installer richiede The Bourne Conspiracy (ID titolo 565507D4).'),
    'The C++ build tools (Visual Studio Build Tools with Clang) aren\'t installed.\nTick "Allow required components to be downloaded and installed" and try again.': (
        'Die C++-Build-Tools (Visual Studio Build Tools mit Clang) sind nicht installiert.\nSetze den Haken bei "Herunterladen und Installieren benötigter Komponenten erlauben" und versuche es erneut.',
        "Les outils de compilation C++ (Visual Studio Build Tools avec Clang) ne sont pas installés.\nCochez « Autoriser le téléchargement et l'installation des composants nécessaires » et réessayez.",
        'Las herramientas de compilación de C++ (Visual Studio Build Tools con Clang) no están instaladas.\nMarca "Permitir descargar e instalar los componentes necesarios" y vuelve a intentarlo.',
        'Gli strumenti di compilazione C++ (Visual Studio Build Tools con Clang) non sono installati.\nSeleziona "Consenti il download e l\'installazione dei componenti necessari" e riprova.'),
    'Installing Visual Studio Build Tools failed or was cancelled (code {code}).': (
        'Die Installation der Visual Studio Build Tools ist fehlgeschlagen oder wurde abgebrochen (Code {code}).',
        "L'installation de Visual Studio Build Tools a échoué ou a été annulée (code {code}).",
        'La instalación de Visual Studio Build Tools falló o se canceló (código {code}).',
        "L'installazione di Visual Studio Build Tools non è riuscita o è stata annullata (codice {code})."),
    "Build Tools finished installing but couldn't be found. Restart your PC and run the installer again.": (
        'Die Build Tools wurden installiert, aber nicht gefunden. Starte deinen PC neu und führe den Installer erneut aus.',
        "Build Tools s'est installé mais reste introuvable. Redémarrez votre PC et relancez l'installateur.",
        'Build Tools se instaló pero no se encuentra. Reinicia el PC y vuelve a ejecutar el instalador.',
        "Build Tools è stato installato ma non viene trovato. Riavvia il PC ed esegui di nuovo l'installer."),
    "The bundled ReXGlue SDK doesn't have the expected layout.": (
        'Das mitgelieferte ReXGlue SDK hat nicht den erwarteten Aufbau.',
        "Le SDK ReXGlue fourni n'a pas la structure attendue.",
        'El SDK de ReXGlue incluido no tiene la estructura esperada.',
        "L'SDK ReXGlue incluso non ha la struttura prevista."),
    'The ReXGlue SDK needs to be downloaded.\nTick "Allow required components to be downloaded and installed" and try again.': (
        'Das ReXGlue SDK muss heruntergeladen werden.\nSetze den Haken bei "Herunterladen und Installieren benötigter Komponenten erlauben" und versuche es erneut.',
        "Le SDK ReXGlue doit être téléchargé.\nCochez « Autoriser le téléchargement et l'installation des composants nécessaires » et réessayez.",
        'Hay que descargar el SDK de ReXGlue.\nMarca "Permitir descargar e instalar los componentes necesarios" y vuelve a intentarlo.',
        'L\'SDK ReXGlue deve essere scaricato.\nSeleziona "Consenti il download e l\'installazione dei componenti necessari" e riprova.'),
    "The downloaded ReXGlue SDK doesn't have the expected layout.": (
        'Das heruntergeladene ReXGlue SDK hat nicht den erwarteten Aufbau.',
        "Le SDK ReXGlue téléchargé n'a pas la structure attendue.",
        'El SDK de ReXGlue descargado no tiene la estructura esperada.',
        "L'SDK ReXGlue scaricato non ha la struttura prevista."),
    'Installer is missing its project files (payload). Re-download the installer.': (
        'Dem Installer fehlen seine Projektdateien (Payload). Lade den Installer erneut herunter.',
        "Il manque à l'installateur ses fichiers de projet (payload). Retéléchargez l'installateur.",
        'Al instalador le faltan sus archivos de proyecto (payload). Vuelve a descargar el instalador.',
        "Mancano i file di progetto dell'installer (payload). Scarica di nuovo l'installer."),
    'The ReXGlue SDK is missing bin\\rexglue.exe.': (
        'Dem ReXGlue SDK fehlt bin\\rexglue.exe.',
        'Le SDK ReXGlue ne contient pas bin\\rexglue.exe.',
        'Al SDK de ReXGlue le falta bin\\rexglue.exe.',
        "Nell'SDK ReXGlue manca bin\\rexglue.exe."),
    'Code generation finished but produced no sources. Click "Open log" for details.': (
        'Die Codegenerierung ist fertig, hat aber keine Quellen erzeugt. Klicke auf "Log öffnen" für Details.',
        "La génération de code est terminée mais n'a produit aucune source. Cliquez sur « Ouvrir le journal » pour les détails.",
        'La generación de código terminó pero no produjo fuentes. Haz clic en "Abrir registro" para ver los detalles.',
        'La generazione del codice è terminata ma non ha prodotto sorgenti. Fai clic su "Apri il log" per i dettagli.'),
    'The build finished but tbc.exe wasn\'t produced. Click "Open log" for details.': (
        'Der Build ist fertig, aber tbc.exe wurde nicht erzeugt. Klicke auf "Log öffnen" für Details.',
        "La compilation est terminée mais tbc.exe n'a pas été produit. Cliquez sur « Ouvrir le journal » pour les détails.",
        'La compilación terminó pero no se generó tbc.exe. Haz clic en "Abrir registro" para ver los detalles.',
        'La compilazione è terminata ma tbc.exe non è stato prodotto. Fai clic su "Apri il log" per i dettagli.'),
    'Something unexpected went wrong: {error}\nClick "Open log" for details.': (
        'Etwas Unerwartetes ist schiefgelaufen: {error}\nKlicke auf "Log öffnen" für Details.',
        "Une erreur inattendue s'est produite : {error}\nCliquez sur « Ouvrir le journal » pour les détails.",
        'Algo inesperado salió mal: {error}\nHaz clic en "Abrir registro" para ver los detalles.',
        'Si è verificato un errore imprevisto: {error}\nFai clic su "Apri il log" per i dettagli.'),
    'Launcher language': (
        'Launcher-Sprache',
        'Langue du lanceur',
        'Idioma del lanzador',
        'Lingua del launcher'),
    'Same as game': (
        'Wie das Spiel',
        'Comme le jeu',
        'Igual que el juego',
        'Come il gioco'),
    'Translated by AI, not yet checked by native speakers.': (
        'Von KI übersetzt, noch nicht von Muttersprachlern geprüft.',
        'Traduit par IA, pas encore vérifié par des locuteurs natifs.',
        'Traducido por IA, aún no revisado por hablantes nativos.',
        "Tradotto dall'IA, non ancora verificato da madrelingua."),
    'Game language': (
        'Spielsprache',
        'Langue du jeu',
        'Idioma del juego',
        'Lingua del gioco'),
    'Aim assist': (
        'Zielhilfe',
        'Aide à la visée',
        'Asistencia de apuntado',
        'Mira assistita'),
    "Smoothing: how much of the game's camera easing stays with mouse look (lower is more responsive). Aiming sensitivity applies over the shoulder and when shooting from cover. Aim assist: the game's sticky aim, made for a controller; off by default with keyboard and mouse (for a controller used alongside too).": (
        'Glättung: wie viel der Kameraverzögerung des Spiels bei Maussteuerung bleibt (weniger reagiert direkter). Die Zielempfindlichkeit gilt beim Zielen über die Schulter und beim Schießen aus der Deckung. Zielhilfe: das klebrige Zielen des Spiels, für Controller gemacht; mit Tastatur und Maus standardmäßig aus (auch für einen zusätzlich genutzten Controller).',
        "Lissage : la part de l'amortissement de caméra du jeu conservée avec la souris (moins = plus réactif). La sensibilité de visée s'applique en visée par-dessus l'épaule et en tirant depuis un abri. Aide à la visée : la visée collante du jeu, conçue pour une manette ; désactivée par défaut avec clavier et souris (y compris pour une manette utilisée en même temps).",
        'Suavizado: cuánto del suavizado de cámara del juego se mantiene con el ratón (menos es más directo). La sensibilidad al apuntar se aplica al apuntar sobre el hombro y al disparar desde cobertura. Asistencia de apuntado: el apuntado pegajoso del juego, pensado para mando; desactivada por defecto con teclado y ratón (también para un mando usado a la vez).',
        "Smussamento: quanta parte dell'ammorbidimento della telecamera del gioco resta con il mouse (meno è più reattivo). La sensibilità di mira vale mirando sopra la spalla e sparando dalla copertura. Mira assistita: la mira 'appiccicosa' del gioco, pensata per il controller; disattivata di default con tastiera e mouse (anche per un controller usato insieme)."),
    "Above 60 is experimental: some scripted scenes can stall. For smoother motion, keep 60 and turn on your graphics driver's frame generation (NVIDIA Smooth Motion, AMD Fluid Motion Frames); see the README.": (
        'Über 60 ist experimentell: Manche geskripteten Szenen können hängen bleiben. Für flüssigere Bewegung bleib bei 60 und schalte die Frame-Generierung deines Grafiktreibers ein (NVIDIA Smooth Motion, AMD Fluid Motion Frames); siehe README.',
        "Au-delà de 60, c'est expérimental : certaines scènes scriptées peuvent se bloquer. Pour un mouvement plus fluide, restez à 60 et activez la génération d'images de votre pilote graphique (NVIDIA Smooth Motion, AMD Fluid Motion Frames) ; voir le README.",
        'Por encima de 60 es experimental: algunas escenas programadas pueden atascarse. Para un movimiento más fluido, deja 60 y activa la generación de fotogramas de tu controlador gráfico (NVIDIA Smooth Motion, AMD Fluid Motion Frames); consulta el README.',
        'Oltre 60 è sperimentale: alcune scene scriptate possono bloccarsi. Per un movimento più fluido, resta a 60 e attiva la generazione di fotogrammi del driver grafico (NVIDIA Smooth Motion, AMD Fluid Motion Frames); vedi il README.'),
    'config unchanged': (
        'Konfiguration unverändert',
        'configuration inchangée',
        'configuración sin cambios',
        'configurazione invariata'),
    'Takedown (when the adrenaline meter has a tier filled)': (
        'Takedown (wenn eine Stufe der Adrenalinanzeige gefüllt ist)',
        "Neutralisation (quand un niveau de la jauge d'adrénaline est rempli)",
        'Derribo (cuando un nivel del medidor de adrenalina está lleno)',
        "Atterramento (quando un livello dell'indicatore di adrenalina è pieno)"),
    "The installer reads your ISO and builds the game on this PC. If this PC doesn't have Microsoft's C++ build tools yet, it downloads and installs them first (Visual Studio Build Tools, several GB; Windows asks for administrator permission), so the first install can take a while.": (
        "Der Installer liest deine ISO und erstellt das Spiel auf diesem PC. Fehlen auf diesem PC noch Microsofts C++-Build-Tools, lädt er sie zuerst herunter und installiert sie (Visual Studio Build Tools, mehrere GB; Windows fragt nach Administratorrechten), daher kann die erste Installation eine Weile dauern.",
        "L'installateur lit votre ISO et compile le jeu sur ce PC. Si ce PC n'a pas encore les outils de compilation C++ de Microsoft, il les télécharge et les installe d'abord (Visual Studio Build Tools, plusieurs Go ; Windows demande l'autorisation d'administrateur), la première installation peut donc prendre un moment.",
        "El instalador lee tu ISO y compila el juego en este PC. Si este PC aún no tiene las herramientas de compilación de C++ de Microsoft, primero las descarga e instala (Visual Studio Build Tools, varios GB; Windows pide permiso de administrador), así que la primera instalación puede tardar un rato.",
        "L'installer legge la tua ISO e compila il gioco su questo PC. Se questo PC non ha ancora gli strumenti di compilazione C++ di Microsoft, prima li scarica e li installa (Visual Studio Build Tools, diversi GB; Windows chiede l'autorizzazione di amministratore), quindi la prima installazione può richiedere un po' di tempo."),
    'CMake wasn\'t found. Open the Visual Studio Installer, click Modify on your Visual Studio, tick "C++ CMake tools for Windows" and install, then try again.': (
        'CMake wurde nicht gefunden. Öffne den Visual Studio Installer, klicke bei deinem Visual Studio auf "Ändern", setze den Haken bei "C++-CMake-Tools für Windows", installiere und versuche es dann erneut.',
        'CMake est introuvable. Ouvrez Visual Studio Installer, cliquez sur Modifier pour votre Visual Studio, cochez « Outils CMake C++ pour Windows », installez, puis réessayez.',
        'No se encontró CMake. Abre el Instalador de Visual Studio, haz clic en Modificar en tu Visual Studio, marca "Herramientas de CMake de C++ para Windows", instala y vuelve a intentarlo.',
        'CMake non è stato trovato. Apri il Visual Studio Installer, fai clic su Modifica per il tuo Visual Studio, seleziona "Strumenti CMake C++ per Windows", installa e riprova.'),
}
