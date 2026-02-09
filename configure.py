# vim: set sts=2 ts=8 sw=2 tw=99 noet:
import sys
from ambuild2 import run

prep = run.BuildParser(sys.path[0], api='2.2')

# 添加自定义选项，使用 add_argument() 以兼容 argparse
prep.options.add_argument('--sm-path', type=str, dest='sm_path', default=None, help='Path to SourceMod SDK')
prep.options.add_argument('--mms-path', type=str, dest='mms_path', default=None, help='Path to Metamod-Source SDK')
prep.options.add_argument('--hl2sdk-path', type=str, dest='hl2sdk_path', default=None, help='Path to HL2SDK')
prep.options.add_argument('--sdks', type=str, dest='sdks', default='csgo', help='SDKs to build for')
prep.options.add_argument('--targets', type=str, dest='targets', default='x86', help='Target architecture')

prep.Configure()
