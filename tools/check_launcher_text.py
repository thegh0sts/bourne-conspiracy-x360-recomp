"""Lists launcher and installer text that has no translation in installer/launcher_text.py:
every tr("...") / tr_label("...") with literal English in installer/tbc_launcher.py and
installer/tbc_installer.py that isn't in its TEXT table, and any translation whose {placeholders}
differ from the English. Run from the repo root: python tools/check_launcher_text.py
(Settings' choices and labels passed through variables aren't seen here: check those by eye.)"""
import re
import ast, sys
sys.path.insert(0, 'installer')
import launcher_text as t
missing = []
for f in ['installer/tbc_launcher.py', 'installer/tbc_installer.py']:
    tree = ast.parse(open(f, encoding='utf-8').read())
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and getattr(node.func, 'id', None) in ('tr', 'tr_label') and node.args:
            a = node.args[0]
            if isinstance(a, ast.Constant) and isinstance(a.value, str):
                ctx = node.args[1].value if len(node.args) > 1 and isinstance(node.args[1], ast.Constant) else None
                if a.value not in t.TEXT and (ctx, a.value) not in t.CONTEXT:
                    missing.append((f.split('/')[-1], node.lineno, a.value))
for m in missing: print(m)
print(len(missing), "missing")
for k, v in t.TEXT.items():
    for x in v:
        if set(re.findall(r"\{[^}]*\}", k)) != set(re.findall(r"\{[^}]*\}", x)):
            print("placeholders differ:", repr(k)[:70], "->", repr(x)[:70])
