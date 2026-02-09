# vim: set sts=2 ts=8 sw=2 tw=99 noet:
import sys
from ambuild2 import run

prep = run.BuildParser(sys.path[0], api='2.2')

# 添加自定义选项以匹配Actions参数
prep.options.add_option('--sm-path', type='string', dest='sm_path', default=None, help='Path to SourceMod SDK')
prep.options.add_option('--mms-path', type='string', dest='mms_path', default=None, help='Path to Metamod-Source SDK')
prep.options.add_option('--hl2sdk-path', type='string', dest='hl2sdk_path', default=None, help='Path to HL2SDK')
prep.options.add_option('--sdks', type='string', dest='sdks', default='csgo', help='SDKs to build for')
prep.options.add_option('--targets', type='string', dest='targets', default='x86', help='Target architecture')

prep.Configure()
