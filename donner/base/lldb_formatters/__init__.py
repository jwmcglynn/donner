# __init__.py

from .base_formatters import *


def __lldb_init_module(debugger, internal_dict):
    """
    This function is called by LLDB when the module is imported.
    It initializes all formatters.
    """
    from .base_formatters import __lldb_init_module as init_base

    init_base(debugger, internal_dict)


# You can add any package-level constants or utility functions here if needed
PACKAGE_VERSION = "1.0.0"


def get_formatter_info():
    return f"LLDB Formatters for Donner project, version {PACKAGE_VERSION}"
