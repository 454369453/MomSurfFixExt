# vim: set sts=2 ts=8 sw=2 tw=99 noet:
import sys
from ambuild2 import run

prep = run.BuildParser(sys.path[0], api='2.2')
prep.Configure()
