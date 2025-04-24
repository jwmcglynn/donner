from .base_formatters import *


def __lldb_init_module(debugger, internal_dict):
    """
    LLDB calls this automatically when you 'command script import …'.
    We hook the summaries into a private category so we don’t pollute 'default'.
    """
    cat = 'DonnerFormatters'
    debugger.HandleCommand(f'type category define {cat}')
    debugger.HandleCommand(f'type summary add -w {cat} -F donner.base.lldb_formatters.summarize_RcString -x "^donner::RcString$"')
    debugger.HandleCommand(f'type summary add -w {cat} -F donner.base.lldb_formatters.summarize_RcStringOrRef -x "^donner::RcStringOrRef$"')

    debugger.HandleCommand(
        f'type summary add -w {cat} -F donner.base.lldb_formatters.summarize_SmallVector -x "^donner::SmallVector<.*>$"')
    debugger.HandleCommand(
        f'type synthetic add -w {cat} -x "^donner::SmallVector<.*>$"'
        f' -l donner.base.lldb_formatters.SmallVectorSynthProvider')
    
    debugger.HandleCommand(f'type category enable {cat}')


# You can add any package-level constants or utility functions here if needed
PACKAGE_VERSION = "1.0.0"


def get_formatter_info():
    return f"LLDB Formatters for Donner project, version {PACKAGE_VERSION}"
