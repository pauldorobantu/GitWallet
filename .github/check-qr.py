# Reads the QR codes the app wrote with two decoders that have nothing to do with it. Every code has to
# read in ZXing-C++. OpenCV's reader is weaker on large symbols, so it has to read those of the sizes
# the app really shows, and only reports on the rest.
import json
import pathlib
import sys

import cv2
import zxingcpp

folder = pathlib.Path(sys.argv[1])
samples = json.loads((folder / 'samples.json').read_text(encoding='utf-8'))
LARGEST_THE_APP_SHOWS = 10

wrong = []
zxing_read = 0
opencv_read = 0
opencv_asked = 0
masks = set()
detector = cv2.QRCodeDetector()
for sample in samples:
    image = cv2.imread(str(folder / sample['file']), cv2.IMREAD_GRAYSCALE)
    found = zxingcpp.read_barcodes(image, formats=zxingcpp.BarcodeFormat.QRCode)
    texts = [one.text for one in found]
    if texts == [sample['text']]:
        zxing_read += 1
    else:
        wrong.append('ZXing: %s (version %d, level %d, mask %d): %r' % (sample['file'], sample['version'], sample['level'], sample['mask'], texts[:1]))
    masks.add(sample['mask'])
    text, _, _ = detector.detectAndDecode(image)
    if text == sample['text']:
        opencv_read += 1
    if sample['version'] <= LARGEST_THE_APP_SHOWS:
        opencv_asked += 1
        if text != sample['text']:
            wrong.append('OpenCV: %s (version %d, level %d, mask %d): %r' % (sample['file'], sample['version'], sample['level'], sample['mask'], text[:40]))

print('%d codes, %d masks: ZXing read %d, OpenCV read %d (%d of them up to version %d, where it has to)' % (len(samples), len(masks), zxing_read, opencv_read, opencv_asked, LARGEST_THE_APP_SHOWS))
for line in wrong[:40]:
    print('FAIL', line)
sys.exit(1 if wrong else 0)
