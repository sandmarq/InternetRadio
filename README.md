# InternetRadio

Firmware de radio Internet pour une carte **Freenove ESP32-S3 WROOM**.

Le projet fournit une interface tactile sur écran TFT, la lecture de stations
Internet et de fichiers audio depuis une carte SD, ainsi qu'une page Web de
configuration accessible depuis le point d'accès de secours.

## Fonctions

- Lecture de stations Internet depuis un fichier M3U.
- Gestion des stations favorites.
- Lecture des fichiers MP3 et FLAC placés dans `/MP3s` sur la carte SD.
- Affichage des métadonnées ID3 lorsque la station ou le fichier les fournit.
- Réglage du volume et passage automatique à la piste suivante.
- Affichage de l'heure via NTP.
- Écran tactile avec économiseur après une minute d'inactivité.
- Mode point d'accès `Radio_Config` pour configurer l'appareil sans réseau connu.

## Matériel

- Freenove ESP32-S3 WROOM.
- Écran TFT ILI9341 240x320.
- Codec audio ES8311.
- Carte microSD en mode SDMMC.
- Contrôleur tactile I2C.
- Amplificateur commandé par la sortie `AP_ENABLE`.

Les broches utilisées sont définies au début de [src/main.cpp](src/main.cpp).

## Préparation

1. Installer Visual Studio Code avec l'extension PlatformIO.
2. Ouvrir ce dossier dans VS Code.
3. Connecter la carte ESP32-S3.
4. Compiler le projet :

   ```bash
   platformio run --environment freenove_esp32_s3_wroom
   ```

5. Téléverser le firmware depuis PlatformIO ou avec :

   ```bash
   platformio run --target upload --environment freenove_esp32_s3_wroom
   ```

## Configuration de la carte SD

Créer un fichier `config.ini` à la racine de la carte SD :

```ini
[RESEAU]
SSID_1=VOTRE_WIFI
PSK_1=VOTRE_PSK_WIFI_EN_HEXADECIMAL

[SYSTEME]
FUSEAU_HORAIRE=EST5EDT,M3.2.0,M11.1.0
VOLUME_DEFAUT=12
```

Le fichier [config.example.ini](config.example.ini) sert uniquement de modèle.
Le fichier réel `config.ini` est ignoré par Git et ne doit jamais être ajouté
au dépôt : il contient les informations de connexion Wi-Fi.

Les listes de stations sont également stockées sur la carte SD :

- `radiointernet.m3u` pour les stations disponibles.
- `favoris.m3u` pour les stations favorites.
- `/MP3s` pour les fichiers audio locaux.

Si aucun réseau configuré n'est détecté, connecter un téléphone ou un ordinateur
au Wi-Fi `Radio_Config`, puis ouvrir `http://192.168.4.1`.

## Organisation du projet

```text
src/main.cpp       Application principale
src/es8311.*       Pilote du codec audio ES8311
include/lv_conf.h  Configuration LVGL
platformio.ini     Configuration PlatformIO
config.example.ini Exemple de configuration sans secret
```

Les fichiers générés par PlatformIO et les configurations locales sont exclus
du dépôt par [.gitignore](.gitignore).

## Licence

Le code original de ce projet est distribué sous licence **GNU GPL v3**.
Consulter [LICENSE](LICENSE) pour le texte complet.

Cette licence est notamment nécessaire pour rester compatible avec la
bibliothèque `ESP32-audioI2S`, utilisée par le firmware. Les licences des
composants tiers sont récapitulées dans
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), et les notices présentes dans
les fichiers tiers ou dans le code source doivent être conservées.

## Sécurité

Ne jamais publier dans Git :

- `config.ini`;
- les mots de passe Wi-Fi;
- les PSK Wi-Fi réelles;
- des clés privées ou des jetons d'API.

Si un secret est publié par erreur, le révoquer ou le modifier immédiatement,
même après suppression du fichier.