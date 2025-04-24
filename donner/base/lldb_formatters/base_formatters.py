import lldb

def _read_utf8(process: lldb.SBProcess, addr: int, length: int) -> str:
    """Read <length> bytes at <addr> and return a printable UTF-8 string."""
    err = lldb.SBError()
    buf = process.ReadMemory(addr, length, err)
    if err.Fail():
        return f"<unreadable: {err.GetCString()}>"
    try:
        return buf.decode("utf-8", "replace")
    except UnicodeDecodeError:
        return "<binary>"

def summarize_RcString(val, _internal_dict):
    """
    Pretty-printer for donner::RcString.

    Short layout:   data_.short_.shiftedSizeByte >> 1  = length
                    bytes stored inline in data_.short_.data

    Long  layout:   data_.long_.shiftedSize    >> 1  = length
                    data_.long_.data                 = char*
    """
    storage = val.GetChildMemberWithName("data_")

    # Decide between short and long using the LSB of shiftedSizeByte.
    shifted = storage.GetChildMemberWithName("short_")\
                     .GetChildMemberWithName("shiftedSizeByte")
    is_long = (shifted.GetValueAsUnsigned() & 1) == 1

    process = val.GetProcess()

    if not is_long:
        size   = shifted.GetValueAsUnsigned() >> 1
        data   = storage.GetChildMemberWithName("short_")\
                        .GetChildMemberWithName("data")
        addr   = data.GetAddress().GetLoadAddress(val.GetTarget())
        return f"\"{_read_utf8(process, addr, size)}\""
    else:
        long_  = storage.GetChildMemberWithName("long_")
        size   = long_.GetChildMemberWithName("shiftedSize").GetValueAsUnsigned() >> 1
        addr   = long_.GetChildMemberWithName("data").GetValueAsUnsigned()
        return f"\"{_read_utf8(process, addr, size)}\" (owned)"
    
def summarize_RcStringOrRef(val, _internal_dict):
    """
    LLDB summary for donner::RcStringOrRef.

    Handles both libc++ and libstdc++ layouts:

      value_ : std::variant<std::string_view, RcString>
                 └── active index held in {__index_, __index, _M_index, ...}

    For the RcString alternative we reuse summarize_RcString().
    For the string-view alternative we decode pointer/size directly.
    """
    variant = val.GetChildMemberWithName("value_")
    if not variant.IsValid():
        return "<invalid RcStringOrRef>"

    # 1. Determine active index
    idx = None
    for name in ("__index_", "__index", "_M_index", "_Index", "index"):
        child = variant.GetChildMemberWithName(name)
        if child.IsValid():
            idx = child.GetValueAsUnsigned()
            break

    # Fallback: walk the children until we bump into something we recognise.
    if idx is None:
        for i in range(variant.GetNumChildren()):
            ty = variant.GetChildAtIndex(i).GetTypeName()
            if "basic_string_view" in ty or "string_view" in ty:
                idx = 0
                break
            if "RcString" in ty:
                idx = 1
                break

    if idx is None:
        return "<unrecognised variant layout>"

    process = val.GetProcess()

    # 2. RcString alternative
    if idx == 1:
        # Look for the child whose dynamic type contains "RcString".
        rc_alt = None
        for i in range(variant.GetNumChildren()):
            c = variant.GetChildAtIndex(i)
            if "RcString" in c.GetTypeName():
                rc_alt = c
                break
        if rc_alt is None or not rc_alt.IsValid():
            return "<corrupt RcString alt>"
        return summarize_RcString(rc_alt, _internal_dict) + " (owned)"

    # 3. std::string_view alternative

    # Grab whichever child *looks* like a string_view.  Some LLDB versions
    # synthesise the active member; others expose the raw union slot.
    sv = None
    for i in range(variant.GetNumChildren()):
        c = variant.GetChildAtIndex(i)
        if "string_view" in c.GetTypeName():
            sv = c
            break
    if sv is None or not sv.IsValid():
        sv = variant  # last-ditch: hope the variant itself has the members

    # Locate pointer / size (libc++ and libstdc++ spell them differently)
    data_field = None
    size_field = None
    for n in ("__data_", "_M_data", "_M_data_pointer"):
        f = sv.GetChildMemberWithName(n)
        if f.IsValid():
            data_field = f
            break
    for n in ("__size_", "_M_len", "_M_size", "_M_length"):
        f = sv.GetChildMemberWithName(n)
        if f.IsValid():
            size_field = f
            break

    if not data_field.IsValid() or not size_field.IsValid():
        return "<unreadable string_view>"

    addr = data_field.GetValueAsUnsigned()
    size = size_field.GetValueAsUnsigned()
    return f"\"{_read_utf8(process, addr, size)}\" (view)"


def _sv_layout(val):
    """
    Return (size, is_long, addr, elem_type) for a SmallVector SBValue.
    Works for any instantiation donner::SmallVector<T, N>.
    """

    size      = val.GetChildMemberWithName("size_").GetValueAsUnsigned()
    is_long   = val.GetChildMemberWithName("isLong_").GetValueAsUnsigned() != 0
    data_u    = val.GetChildMemberWithName("data_")
    vec_type  = val.GetType()
    elem_type = vec_type.GetTemplateArgumentType(0)
    target    = val.GetTarget()

    if is_long:
        addr = data_u.GetChildMemberWithName("longData").GetValueAsUnsigned()
    else:
        short_data = data_u.GetChildMemberWithName("shortData")
        addr = short_data.GetAddress().GetLoadAddress(target)

    return size, is_long, addr, elem_type

def summarize_SmallVector(val, _internal_dict):
    """
    One-line summary for donner::SmallVector<T,N>.

    * Shows all elements up to 8; otherwise head … tail.
    """
    MAX_INLINE = 8  # print all elements up to this size
    EDGE       = 4  # elements shown at head / tail when truncated

    val = val.GetNonSyntheticValue()

    size, _, addr, elem_type = _sv_layout(val)
    if addr == 0:
        return f"size={size} [<null>]"

    process   = val.GetProcess()
    elem_size = elem_type.GetByteSize()

    def _elem(i):
        ea = addr + i * elem_size
        ev = val.CreateValueFromAddress(f"", ea, elem_type)
        return ev.GetSummary() or ev.GetValue() or "?"

    if size <= MAX_INLINE:
        payload = ", ".join(_elem(i) for i in range(size))
    else:
        head = ", ".join(_elem(i)               for i in range(EDGE))
        tail = ", ".join(_elem(size - EDGE + i) for i in range(EDGE))
        payload = f"{head}, …, {tail}"

    return f"size={size} [{payload}]"

class SmallVectorSynthProvider:
    """
    LLDB synthetic children provider for donner::SmallVector<T,N>.
    Lets you expand the vector and see element [0], [1], … as children.
    """
    def __init__(self, val, _dict):
        self.val = val
        self.update()

    # Called when the underlying object may have changed.
    def update(self):
        self.size, self.is_long, self.addr, self.elem_type = _sv_layout(self.val)
        self.elem_size = self.elem_type.GetByteSize()
        return True

    def num_children(self):
        return self.size

    def has_children(self):
        return self.size > 0

    def get_child_index(self, name):
        # Accept either "[123]" or "123"
        if name.startswith('[') and name.endswith(']'):
            name = name[1:-1]
        try:
            idx = int(name)
            return idx if 0 <= idx < self.size else -1
        except ValueError:
            return -1

    def get_child_at_index(self, idx):
        if idx < 0 or idx >= self.size or self.addr == 0:
            return None
        ea = self.addr + idx * self.elem_size
        return self.val.CreateValueFromAddress(f"[{idx}]", ea, self.elem_type)
