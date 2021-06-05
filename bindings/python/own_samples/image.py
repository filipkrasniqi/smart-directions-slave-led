#!/usr/bin/env python
import time
import sys

from rgbmatrix import RGBMatrix, RGBMatrixOptions
from PIL import Image, ImageSequence

if len(sys.argv) < 2:
    sys.exit("Require an image argument")
else:
    image_file = sys.argv[1]

image = Image.open(image_file)

# Configuration for the matrix
options = RGBMatrixOptions()
options.rows = 32
options.chain_length = 1
options.parallel = 1
options.hardware_mapping = 'adafruit-hat'  # If you have an Adafruit HAT: 'adafruit-hat'

matrix = RGBMatrix(options = options)

print("Number of frames: "+str(image.n_frames))

# Make image fit our screen.
#image.thumbnail((matrix.width, matrix.height), Image.ANTIALIAS)

#matrix.SetImage(image.convert('RGB'))

try:
    print("Press CTRL-C to stop.")
    i = 0
    while True:
        for i in range(image.n_frames):

            image_new = Image.open(image_file)
            image_new.seek(i)
            
            image_new.thumbnail((matrix.width, matrix.height), Image.ANTIALIAS)
            #print(frame.tile)
            to_show = image_new.convert('RGB')
            matrix.SetImage(to_show)
            time.sleep(1)
            matrix.Clear()
except KeyboardInterrupt:
    sys.exit(0)
