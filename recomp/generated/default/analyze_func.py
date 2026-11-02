import re

def read_func(filepath, start_line, func_name):
    """Read a PPC recompiled function from a file."""
    with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
        lines = f.readlines()

    # Find the exact start line (0-indexed)
    start_idx = start_line - 1
    
    # Find the end: next DEFINE_REX_FUNC or end of file
    end_idx = len(lines)
    for i in range(start_idx + 1, len(lines)):
        if 'DEFINE_REX_FUNC' in lines[i]:
            end_idx = i
            break
    
    func_lines = lines[start_idx:end_idx]
    
    # Extract all branch instructions and labels
    branches = []
    labels = set()
    line_nums = []
    
    for i, line in enumerate(func_lines):
        actual_line = start_line + i
        stripped = line.strip()
        
        # Match labels like "loc_8279DF10:"
        if re.match(r'^loc_[0-9A-Fa-f]+:$', stripped):
            labels.add(stripped.rstrip(':'))
            continue
        
        # Match branch instructions
        # Patterns: "bne cr6,0x...", "beq cr6,0x...", "blt cr6,0x...", 
        #           "ble cr6,0x...", "bgt cr6,0x...", "bge cr6,0x...",
        #           "b 0x...", "bl 0x...", "blr", etc.
        
        # Extract the actual branch target from comments
        if '//' in stripped:
            comment = stripped.split('//')[-1].strip()
            
            # Conditional branches
            m = re.search(r'\b(bne|beq|blt|ble|bgt|bge|bdnzf|bdz)\s+(cr\d+,)?\s*(0x[0-9A-Fa-f]+)', comment)
            if m:
                target = m.group(3)
                branches.append((actual_line, comment, target))
                line_nums.append(actual_line)
                continue
            
            # Unconditional branches
            m = re.search(r'\b(b\s+0x[0-9A-Fa-f]+)\b', comment)
            if m:
                target = re.search(r'0x[0-9A-Fa-f]+', comment).group(0)
                branches.append((actual_line, comment, target))
                line_nums.append(actual_line)
                continue
            
            # Function calls (bl)
            m = re.search(r'\b(bl\s+0x[0-9A-Fa-f]+)\b', comment)
            if m:
                target = re.search(r'0x[0-9A-Fa-f]+', comment).group(0)
                branches.append((actual_line, comment + ' [CALL]', target))
                line_nums.append(actual_line)
    
    return func_lines, branches, labels, line_nums

if __name__ == '__main__':
    import sys
    
    filepath = 'J:/XBOX360 port/Transformers Revenge of the Fallen/recomp/generated/default/T2ROTF_recomp.0.cpp'
    start_line = 81212
    func_name = 'sub_8279D6D8'
    
    func_lines, branches, labels, line_nums = read_func(filepath, start_line, func_name)
    
    print(f"Function {func_name} spans {len(func_lines)} lines ({start_line} to {start_line + len(func_lines) - 1})")
    print(f"\nTotal branches/calls found: {len(branches)}")
    print("=" * 80)
    
    # Find the call to sub_827FCA18
    call_fca18 = None
    for i, line in enumerate(func_lines):
        actual_line = start_line + i
        if 'sub_827FCA18' in line:
            call_fca18 = actual_line
            break
    
    print(f"\nCall to sub_827FCA18 at line: {call_fca18}")
    print("=" * 80)
    
    # Print all branches before the call to sub_827FCA18
    print("\nAll branches/calls before sub_827FCA18 call:")
    print("-" * 80)
    for ln, comment, target in branches:
        prefix = ">>> " if ln == call_fca18 else "    "
        print(f"{prefix}Line {ln}: {comment} -> {target}")
    
    # Find early returns
    print("\n" + "=" * 80)
    print("Potential early return points (blr before sub_827FCA18 call):")
    print("-" * 80)
    for i, line in enumerate(func_lines):
        actual_line = start_line + i
        if actual_line >= call_fca18:
            break
        if '// blr' in line or 'return;' in line:
            # Check context
            context_start = max(0, i - 3)
            context_end = min(len(func_lines), i + 2)
            print(f"\nLine {actual_line}:")
            for j in range(context_start, context_end):
                marker = ">>> " if j == i else "    "
                print(f"{marker}{func_lines[j].rstrip()}")
