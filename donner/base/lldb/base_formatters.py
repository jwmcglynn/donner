# Provides custom lldb formatters for the donner base library.
import lldb


def is_long_string(sizeByte):
    return sizeByte & 1 == 1


def decode_string_size(size):
    return size >> 1


def read_string(value, size):
    error = lldb.SBError()
    result = value.ReadRawData(error, 0, size)
    if error.Fail():
        return "<error:" + error.GetCString() + ">"
    else:
        return '"' + result.decode("utf-8") + '"'


def summarize_RcString(value, internal_dict):
    union = value.GetChildMemberWithName("data_")
    short = union.GetChildMemberWithName("short_")
    sizeByte = short.GetChildMemberWithName(
        "shiftedSizeByte").GetValueAsUnsigned()

    if is_long_string(sizeByte):
        long = union.GetChildMemberWithName("long_")
        size = decode_string_size(
            long.GetChildMemberWithName("shiftedSize").GetValueAsUnsigned())
        result_data = long.GetChildMemberWithName(
            "data").GetPointeeData(0, size)

        return read_string(result_data, size)
    else:
        result_data = short.GetChildMemberWithName(
            "data").GetData()

        return read_string(result_data, decode_string_size(sizeByte))


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand(
        'type summary add -F base_formatters.summarize_RcString "donner::RcString" -w donner')
    debugger.HandleCommand("type category enable donner")
