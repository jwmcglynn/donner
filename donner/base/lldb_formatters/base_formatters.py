def _debug_dump_children(val, indent=0):
    """
    Recursively dumps a value's children to the LLDB console, for debugging.
    """
    prefix = "  " * indent
    num_children = val.GetNumChildren()
    print(f"[formatter debug] {prefix}{val.GetName() or '(unnamed)'}: "
          f"type={val.GetTypeName() or '??'}, #children={num_children}")
    for i in range(num_children):
        child = val.GetChildAtIndex(i)
        print(f"[formatter debug] {prefix}  Child[{i}]: name={child.GetName()}, type={child.GetTypeName()}")
        _debug_dump_children(child, indent+2)

def _search_for_index_member(val):
    """
    Recursively searches all children of 'val' for a known variant index field name:
    - '__index'
    - '_M_index'
    - or containing substring 'index'

    Returns (True, child) if found, or (False, None) if not found.
    """
    # Check this value for match
    name = val.GetName() or ""
    if (name == "__index") or ("_M_index" in name) or ("index" in name):
        return (True, val)

    # Check direct children
    n = val.GetNumChildren()
    for i in range(n):
        child = val.GetChildAtIndex(i)
        cname = child.GetName() or ""
        if (cname == "__index") or ("_M_index" in cname) or ("index" in cname):
            return (True, child)

    # If not found, search deeper
    for i in range(n):
        child = val.GetChildAtIndex(i)
        (found, cval) = _search_for_index_member(child)
        if found:
            return (True, cval)

    return (False, None)

def summarize_RcStringOrRef(value, internal_dict):
    """
    Enhanced summarizer with a deep search for the variant index,
    plus child dumping to debug how the variant is stored.
    """
    print("[formatter debug] summarize_RcStringOrRef: start")
    try:
        variant = value.GetChildMemberWithName("value_")
        if not variant.IsValid():
            print("[formatter debug] summarize_RcStringOrRef: variant is invalid, dumping children of 'value_' anyway:")
            _debug_dump_children(value)  # See if it truly has no valid 'value_'
            return "<error: cannot access value_>"

        # Let's dump the top-level structure to see how it's laid out
        print("[formatter debug] summarize_RcStringOrRef: top-level variant child dump:")
        _debug_dump_children(variant, indent=1)

        # Attempt to discover the variant index by searching
        (found_index, index_val) = _search_for_index_member(variant)
        if not found_index:
            print("[formatter debug] summarize_RcStringOrRef: cannot find any variant index member in top-level or nested children.")
            return "<error: cannot determine variant index>"

        index = index_val.GetValueAsUnsigned()
        print(f"[formatter debug] summarize_RcStringOrRef: found index -> {index}")

        # Index 0: holds std::string_view, index 1: holds RcString (per your class definition)
        if index == 0:
            print("[formatter debug] summarize_RcStringOrRef: index=0 => std::string_view path")
            # Attempt to find the child that holds the actual string_view
            str_view = None
            n_kids = variant.GetNumChildren()
            for i in range(n_kids):
                child = variant.GetChildAtIndex(i)
                tn = child.GetTypeName() or ""
                if "string_view" in tn:
                    print(f"[formatter debug] summarize_RcStringOrRef: found child i={i} with typeName={tn}")
                    str_view = child
                    break

            if not str_view:
                # Possibly stored in a __data or __impl union
                data_union = variant.GetChildMemberWithName("__data")
                if data_union.IsValid():
                    # Dump to see what's inside
                    print("[formatter debug] variant has __data, dumping its children:")
                    _debug_dump_children(data_union, indent=2)
                    for c_i in range(data_union.GetNumChildren()):
                        c_c = data_union.GetChildAtIndex(c_i)
                        tn = c_c.GetTypeName() or ""
                        if "string_view" in tn:
                            print(f"[formatter debug] found string_view in __data child i={c_i}")
                            str_view = c_c
                            break

            if not str_view:
                print("[formatter debug] summarize_RcStringOrRef: cannot locate string_view data after searching __data as well.")
                return "<error: cannot locate string_view data>"

            # Now parse the string_view's __data_ pointer and __size_ field or equivalents
            data_ptr = None
            size_val = 0
            if str_view.GetChildMemberWithName("__data_").IsValid():
                data_ptr = str_view.GetChildMemberWithName("__data_")
                size_val = str_view.GetChildMemberWithName("__size_").GetValueAsUnsigned()
            elif str_view.GetChildMemberWithName("_M_str").IsValid():
                data_ptr = str_view.GetChildMemberWithName("_M_str")
                size_val = str_view.GetChildMemberWithName("_M_len").GetValueAsUnsigned()
            else:
                # Fall back to scanning children
                str_view_kids = str_view.GetNumChildren()
                print("[formatter debug] scanning string_view's children for data pointer or size:")
                for k in range(str_view_kids):
                    c = str_view.GetChildAtIndex(k)
                    cname = c.GetName() or ""
                    ctype = c.GetTypeName() or ""
                    print(f"[formatter debug]   child {k}: name={cname}, type={ctype}")
                    if c.GetType().GetTypeClass() == lldb.eTypeClassPointer:
                        data_ptr = c
                    elif "size" in cname or "size" in ctype or "_M_len" in cname or "_M_len" in ctype:
                        size_val = c.GetValueAsUnsigned()

            print(f"[formatter debug] summarize_RcStringOrRef: string_view size={size_val}")
            if not data_ptr or not data_ptr.IsValid():
                print("[formatter debug] summarize_RcStringOrRef: no valid data pointer found in string_view")
                return "<error: cannot locate string_view data pointer>"
            if size_val == 0:
                return "empty (view)"

            process = value.GetProcess()
            if not process.IsValid():
                print("[formatter debug] summarize_RcStringOrRef: invalid process")
                return "<error: invalid process>"
            addr = data_ptr.GetValueAsUnsigned()
            if addr == 0:
                print("[formatter debug] summarize_RcStringOrRef: data pointer is 0")
                return "<error: null string pointer>"

            error = lldb.SBError()
            memory = process.ReadMemory(addr, size_val, error)
            if error.Fail():
                err_msg = error.GetCString()
                print(f"[formatter debug] summarize_RcStringOrRef: error reading memory -> {err_msg}")
                return f"<error reading memory: {err_msg}>"

            try:
                string_parts = []
                has_nuls = False
                for i in range(size_val):
                    byte = memory[i : i + 1]
                    if byte == b"\0":
                        has_nuls = True
                        string_parts.append("\\0")
                    else:
                        try:
                            string_parts.append(byte.decode("utf-8"))
                        except UnicodeDecodeError:
                            string_parts.append(f"\\x{ord(byte):02x}")
                string_data = "".join(string_parts)
                if has_nuls:
                    return f"\"{string_data}\" (view, contains NUL)"
                else:
                    return f"\"{string_data}\" (view)"
            except Exception as ex:
                print(f"[formatter debug] summarize_RcStringOrRef: exception decoding memory: {ex}")
                return f"<binary data size={size_val}> (view)"
        else:
            print("[formatter debug] summarize_RcStringOrRef: index=1 => RcString (owned)")
            # We expect a child that looks like RcString
            rc_string = None
            for i in range(variant.GetNumChildren()):
                c = variant.GetChildAtIndex(i)
                tn = c.GetTypeName() or ""
                if "RcString" in tn:
                    print(f"[formatter debug] summarize_RcStringOrRef: found child {i} => {tn}")
                    rc_string = c
                    break

            if not rc_string:
                # Possibly stored in __data union
                data_union = variant.GetChildMemberWithName("__data")
                if data_union.IsValid():
                    print("[formatter debug] searching __data for RcString child:")
                    for j in range(data_union.GetNumChildren()):
                        c2 = data_union.GetChildAtIndex(j)
                        tn2 = c2.GetTypeName() or ""
                        print(f"[formatter debug]   child {j}: type={tn2}")
                        if "RcString" in tn2:
                            rc_string = c2
                            break

            if not rc_string:
                print("[formatter debug] summarize_RcStringOrRef: cannot locate RcString data in variant")
                return "<error: cannot locate RcString data>"

            # Defer to the RcString summarizer
            from . import lldb_formatters  # or whichever module name
            result = summarize_RcString(rc_string, internal_dict)
            return result + " (owned)"

    except Exception as e:
        print(f"[formatter debug] summarize_RcStringOrRef: top-level exception -> {e}")
        return f"<formatter error: {str(e)}>"
