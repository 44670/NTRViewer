# NTRViewer
PC Viewer for NTR CFW's streaming feature

# Dependencies
SDL2: https://www.libsdl.org/download-2.0.php

libjpeg-turbo: http://libjpeg-turbo.virtualgl.org/

ffmpeg(libswscale): http://ffmpeg.org/

# License
GPLv3


# Build Guide
OSX:

brew install sdl jpeg-turbo ffmpeg

make CONF=OSX


Linux:

apt-get install libsdl1.2-dev libjpeg-turbo8-dev libswscale

make CONF=Release


Windows:

Follow the guide in ffmpeg directory, then build this project with Visual Studio 2013.
