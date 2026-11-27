# SPDX-License-Identifier: MIT
import sys, re
# Removes __APPLE__-only code: keeps the non-Apple side of #ifdef/#ifndef/#if defined(__APPLE__) blocks.
out=[]; stack=[]  # each: (is_apple_block, keep_current, parent_keep)
keep=True
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    t=line.strip()
    m=re.match(r'#\s*(ifdef|ifndef|if)\s*(.*)', t)
    if m:
        kind, cond = m.groups()
        apple = (kind in ('ifdef','ifndef') and cond.split()[0]=='__APPLE__') or (kind=='if' and re.fullmatch(r'(!\s*)?defined\s*\(?\s*__APPLE__\s*\)?', cond.strip()))
        if apple:
            neg = kind=='ifndef' or cond.strip().startswith('!')
            branch = neg  # keep when not apple
            stack.append(('A', keep, branch)); keep = keep and branch; continue
        stack.append(('O', keep, None))
        if keep: out.append(line)
        continue
    if re.match(r'#\s*else', t) and stack and stack[-1][0]=='A':
        k, parent, branch = stack[-1]; stack[-1]=(k,parent,not branch); keep = parent and not branch; continue
    if re.match(r'#\s*elif', t) and stack and stack[-1][0]=='A':
        sys.exit("elif in apple block: "+line)
    if re.match(r'#\s*endif', t) and stack:
        k, parent, _ = stack.pop()
        if k=='A': keep=parent; continue
        if keep: out.append(line)
        continue
    if keep: out.append(line)
    else: out.append('\n')
open(sys.argv[2],'w').write(''.join(out))
