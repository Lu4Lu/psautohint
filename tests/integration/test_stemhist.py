from __future__ import print_function, division, absolute_import

import glob
from os.path import basename
import pytest

from fontTools.misc.xmlWriter import XMLWriter
from fontTools.cffLib import CFFFontSet
from fontTools.ttLib import TTFont
from psautohint.autohint import ACOptions, hintFiles

from .differ import main as differ
from . import DATA_DIR


class Options(ACOptions):
    def __init__(self, inpath, outpath, zones, stems, all_stems):
        super(Options, self).__init__()
        self.inputPaths = [inpath]
        self.outputPaths = [outpath]
        self.hintAll = True
        self.verbose = False
        self.report_alignment_zones = zones
        self.report_stem_widths = stems
        self.report_all_stems = all_stems


@pytest.mark.parametrize("otf", glob.glob("%s/*/*/font.otf" % DATA_DIR))
@pytest.mark.parametrize("zones,stems,all_stems", [
    pytest.param(True, False, False, id="report_alignment_zones"),
    pytest.param(True, False, True, id="report_alignment_zones,all_stems"),
    pytest.param(False, True, False, id="report_stem_widths"),
    pytest.param(False, True, True, id="report_stem_widths,all_stems"),
])
def test_otf(otf, zones, stems, all_stems, tmpdir):
    out = str(tmpdir / basename(otf)) + ".out"
    options = Options(otf, out, zones, stems, all_stems)
    hintFiles(options)
