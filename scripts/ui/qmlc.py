#!/usr/bin/env python3
"""Compile the supported Ame.Ui QML subset into freestanding C++ and metadata."""
from __future__ import annotations
import argparse
from dataclasses import dataclass, field
import json
from pathlib import Path
import re
import sys


class Diagnostic(Exception):
    pass


@dataclass
class Token:
    value: str
    line: int
    column: int
    kind: str = ''


@dataclass
class Expr:
    token: Token
    op: str
    args: list = field(default_factory=list)


@dataclass
class Object:
    token: Token
    kind: str
    parent: int
    name: str = ''
    bindings: dict = field(default_factory=dict)
    handlers: dict = field(default_factory=dict)
    fill: bool = False
    center: object = None
    margins: Expr | None = None


@dataclass
class Property:
    token: Token
    name: str
    type: str
    value: Expr
    readonly: bool


KINDS = {'Item', 'Rectangle', 'Text', 'Row', 'Column', 'Button', 'MouseArea'}
ATTRS = {'x': ('int', 'X'), 'y': ('int', 'Y'), 'z': ('int', 'Z'), 'width': ('int', 'Width'),
         'height': ('int', 'Height'), 'spacing': ('int', 'Spacing'),
         'padding': ('int', 'Padding'), 'color': ('color', 'Color'),
         'visible': ('bool', 'Visible'), 'enabled': ('bool', 'Enabled'),
         'clip': ('bool', 'Clip'), 'text': ('string', 'text')}
CPP = {'int': 'int32_t', 'bool': 'bool', 'string': 'ame::ui::Text', 'color': 'uint32_t'}
PRECEDENCE = {'||': 1, '&&': 2, '==': 3, '!=': 3, '<': 4, '>': 4,
              '<=': 4, '>=': 4, '+': 5, '-': 5, '*': 6}


class Compiler:
    def __init__(self, source: str, filename: str, name: str):
        self.source, self.filename, self.name = source, filename, name
        self.tokens = []
        self.pos = 0
        self.objects = []
        self.properties = {}
        self.ids = {}
        self.checked = {}
        self.order = []
        self.expression_depth = 0
        pattern = re.compile(r'(?P<comment>//[^\n]*|/\*[\s\S]*?\*/)|'
                             r'(?P<string>"(?:[^"\\\n]|\\.)*")|'
                             r'(?P<number>\d+)|(?P<id>[A-Za-z_][A-Za-z_0-9]*)|'
                             r'(?P<op>\+\+|--|\+=|-=|==|!=|<=|>=|&&|\|\||[{}():;.,?+*!<>=-])|'
                             r'(?P<newline>\n)|(?P<space>[ \t\r]+)|(?P<bad>.)')
        line, column = 1, 1
        for match in pattern.finditer(source):
            value, kind = match.group(), match.lastgroup
            token = Token(value, line, column, kind)
            if kind == 'bad': self.fail(token, f'unexpected character {value!r}')
            if kind not in ('comment', 'space'): self.tokens.append(token)
            if '\n' in value:
                line += value.count('\n'); column = len(value.rsplit('\n', 1)[-1]) + 1
            else: column += len(value)
        self.tokens.append(Token('<eof>', line, column))

    def fail(self, token, message):
        raise Diagnostic(f'{self.filename}:{token.line}:{token.column}: error: {message}')

    def peek(self, value=None):
        token = self.tokens[self.pos]
        return token.value == value if value is not None else token

    def take(self, value=None):
        token = self.peek()
        if value is not None and token.value != value:
            self.fail(token, f'expected {value!r}, got {token.value!r}')
        self.pos += 1
        return token

    def identifier(self):
        token = self.take()
        if token.kind != 'id': self.fail(token, 'expected identifier')
        return token

    def separators(self):
        while self.peek('\n') or self.peek(';'): self.take()

    def end_member(self):
        if not self.peek('}') and not self.peek('<eof>') and not self.peek('\n') and not self.peek(';'):
            self.fail(self.peek(), 'expected newline or semicolon after member')
        self.separators()

    def expression(self, level=0):
        self.expression_depth += 1
        if self.expression_depth > 64:
            self.fail(self.peek(), 'expression exceeds 64 nesting levels')
        try:
            return self.expression_body(level)
        finally:
            self.expression_depth -= 1

    def expression_body(self, level=0):
        token = self.take()
        if token.value in ('!', '-'):
            left = Expr(token, 'unary', [token.value, self.expression(7)])
        elif token.value == '(':
            left = self.expression(); self.take(')')
        elif token.kind in ('number', 'string') or token.value in ('true', 'false'):
            left = Expr(token, 'literal')
        elif token.kind == 'id':
            names = [token.value]
            while self.peek('.'):
                self.take(); names.append(self.identifier().value)
            if self.peek('('):
                self.take(); argument = self.expression(); self.take(')')
                left = Expr(token, 'call', ['.'.join(names), argument])
            else: left = Expr(token, 'ref', names)
        else: self.fail(token, 'expected binding expression')
        while PRECEDENCE.get(self.peek().value, -1) >= level:
            operator = self.take()
            right = self.expression(PRECEDENCE[operator.value] + 1)
            left = Expr(operator, 'binary', [operator.value, left, right])
        if level == 0 and self.peek('?'):
            operator = self.take(); yes = self.expression(); self.take(':'); no = self.expression()
            left = Expr(operator, 'conditional', [left, yes, no])
        return left

    def parse(self):
        self.separators()
        self.take('import'); module = self.identifier().value
        self.take('.'); module += '.' + self.identifier().value
        if module != 'Ame.Ui': self.fail(self.peek(), 'supported module is Ame.Ui 1.0')
        self.take('1'); self.take('.'); self.take('0'); self.end_member()
        self.object(-1, 0)
        self.separators(); self.take('<eof>')

    def object(self, parent, depth):
        token = self.identifier()
        if token.value not in KINDS: self.fail(token, f'unsupported QML type {token.value!r}')
        if depth >= 12 or len(self.objects) >= 256:
            self.fail(token, 'component exceeds 12 nesting levels or 256 static objects')
        index = len(self.objects)
        obj = Object(token, token.value, parent)
        self.objects.append(obj)
        self.take('{'); self.separators()
        while not self.peek('}'):
            start = self.peek()
            if start.value in ('property', 'readonly'):
                if index: self.fail(start, 'custom properties currently belong on the component root')
                readonly = self.peek('readonly')
                if readonly: self.take()
                self.take('property'); kind = self.identifier(); name = self.identifier()
                if kind.value not in CPP: self.fail(kind, 'supported property types: int, bool, string, color')
                if name.value in self.properties or name.value in ATTRS:
                    self.fail(name, f'duplicate or reserved property {name.value!r}')
                self.take(':'); value = self.expression()
                self.properties[name.value] = Property(name, name.value, kind.value, value, readonly)
                self.end_member(); continue
            key = self.identifier()
            if self.peek('{'):
                self.pos -= 1; self.object(index, depth + 1); self.separators(); continue
            path = key.value
            while self.peek('.'):
                self.take(); path += '.' + self.identifier().value
            self.take(':')
            if path == 'id':
                name = self.identifier()
                if obj.name or name.value in self.ids or name.value in ('parent', 'true', 'false'):
                    self.fail(name, 'duplicate or reserved object id')
                obj.name = name.value; self.ids[name.value] = index
            elif path == 'onClicked':
                if obj.kind not in ('Button', 'MouseArea'): self.fail(key, 'onClicked requires Button or MouseArea')
                if path in obj.handlers: self.fail(key, 'duplicate onClicked handler')
                statements = []
                self.take('{'); self.separators()
                while not self.peek('}'):
                    target = self.identifier(); names = [target.value]
                    if self.peek('.'):
                        self.take(); names.append(self.identifier().value)
                    operator = self.take()
                    if operator.value not in ('=', '+=', '-=', '++', '--'):
                        self.fail(operator, 'handlers support typed property assignments only')
                    value = Expr(Token('1', operator.line, operator.column, 'number'), 'literal') if operator.value in ('++', '--') else self.expression()
                    statements.append((target, names, operator.value, value))
                    self.end_member()
                self.take('}'); obj.handlers[path] = statements
            elif path in ('anchors.fill', 'anchors.centerIn'):
                if obj.fill or obj.center is not None: self.fail(key, 'duplicate or conflicting anchor')
                if parent < 0: self.fail(key, f'root has no parent to {path.split(".")[1]}')
                if path == 'anchors.fill':
                    self.take('parent'); obj.fill = True; self.end_member(); continue
                # A forward id is legal in QML, so resolve after parsing.
                obj.center = self.identifier()
            elif path == 'anchors.margins':
                if obj.margins is not None: self.fail(key, 'duplicate anchors.margins')
                obj.margins = self.expression()
            else:
                if path not in ATTRS: self.fail(key, f'unsupported QML property {path!r}')
                if path in obj.bindings: self.fail(key, f'duplicate binding {path!r}')
                if path == 'text' and obj.kind not in ('Text', 'Button'):
                    self.fail(key, 'text belongs to Text or Button')
                obj.bindings[path] = self.expression()
            self.end_member()
        self.take('}')
        # Anchors and absolute positioning conflict (QML_SPEC A4).
        if obj.fill:
            conflict = next((key for key in ('x', 'y', 'width', 'height') if key in obj.bindings), None)
            if conflict: self.fail(token, f'anchors.fill cannot be combined with {conflict}')
        if obj.center is not None:
            conflict = next((key for key in ('x', 'y') if key in obj.bindings), None)
            if conflict: self.fail(token, f'anchors.centerIn cannot be combined with {conflict}')
        if obj.margins is not None and not (obj.fill or obj.center is not None):
            self.fail(token, 'anchors.margins requires anchors.fill or anchors.centerIn')

    def reference(self, expr, index):
        names = expr.args
        if len(names) == 1:
            if names[0] in self.properties: return ('state', names[0])
            if names[0] in ATTRS: return (index, names[0])
        if len(names) == 2:
            owner, name = names
            target = self.objects[index].parent if owner == 'parent' else self.ids.get(owner, -1)
            if target == 0 and name in self.properties: return ('state', name)
            if target >= 0 and name in ATTRS: return (target, name)
        self.fail(expr.token, f'unknown name {".".join(names)!r}')

    def expression_code(self, expr, index, expected=None):
        token = expr.token
        if expr.op == 'literal':
            if token.kind == 'number':
                value = int(token.value)
                if value > 2147483647: self.fail(token, 'integer literal exceeds int32 range')
                result = ('int', str(value), set())
            elif token.value in ('true', 'false'): result = ('bool', token.value, set())
            else:
                try: value = json.loads(token.value)
                except ValueError: self.fail(token, 'invalid string escape')
                if '\0' in value or len(value.encode('utf-8')) >= 256:
                    self.fail(token, 'string must fit 255 UTF-8 bytes and contain no NUL')
                if expected == 'color':
                    if not re.fullmatch(r'#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?', value):
                        self.fail(token, 'color must be #RRGGBB or #AARRGGBB')
                    color = int(value[1:], 16) | (0xff000000 if len(value) == 7 else 0)
                    result = ('color', f'0x{color:08x}u', set())
                else:
                    encoded = ''.join(f'\\{byte:03o}' for byte in value.encode('utf-8'))
                    result = ('string', f'ame::ui::Text("{encoded}")', set())
        elif expr.op == 'ref':
            key = self.reference(expr, index)
            if key[0] == 'state': kind, code = self.properties[key[1]].type, 'p_' + key[1]
            else:
                obj, name = key
                if name not in self.objects[obj].bindings:
                    self.fail(token, 'references to implicit/default geometry or properties are not supported yet')
                kind = ATTRS[name][0]
                code = f'scene.node({obj}).' + ('label' if name == 'text' else name)
            result = (kind, code, {key})
        elif expr.op == 'call':
            name, argument = expr.args
            if name != 'String': self.fail(token, 'only the pure String(value) conversion is supported')
            kind, code, deps = self.expression_code(argument, index)
            if kind == 'color': self.fail(token, 'String(color) is not supported')
            result = ('string', f'ame::ui::text({code})', deps)
        elif expr.op == 'unary':
            operator, argument = expr.args
            kind, code, deps = self.expression_code(argument, index, 'bool' if operator == '!' else 'int')
            result = (kind, f'(!({code}))' if operator == '!' else f'ame::ui::sub(0, {code})', deps)
        elif expr.op == 'binary':
            operator, left, right = expr.args
            lt, lc, ld = self.expression_code(left, index)
            rt, rc, rd = self.expression_code(right, index)
            if operator == '+' and 'string' in (lt, rt):
                if 'color' in (lt, rt): self.fail(token, 'cannot concatenate a color')
                result = ('string', f'ame::ui::concat(ame::ui::text({lc}), ame::ui::text({rc}))', ld | rd)
            elif operator in ('+', '-', '*') and lt == rt == 'int':
                function = {'+': 'add', '-': 'sub', '*': 'mul'}[operator]
                result = ('int', f'ame::ui::{function}({lc}, {rc})', ld | rd)
            elif operator in ('==', '!=') and lt == rt:
                result = ('bool', f'({lc} {operator} {rc})', ld | rd)
            elif operator in ('<', '>', '<=', '>=') and lt == rt == 'int':
                result = ('bool', f'({lc} {operator} {rc})', ld | rd)
            elif operator in ('&&', '||') and lt == rt == 'bool':
                result = ('bool', f'({lc} {operator} {rc})', ld | rd)
            else: self.fail(token, f'operator {operator!r} does not accept {lt} and {rt}')
        elif expr.op == 'conditional':
            condition, yes, no = expr.args
            _, cc, cd = self.expression_code(condition, index, 'bool')
            yt, yc, yd = self.expression_code(yes, index, expected)
            nt, nc, nd = self.expression_code(no, index, yt)
            result = (yt, f'({cc} ? {yc} : {nc})', cd | yd | nd)
        else: self.fail(token, 'unsupported expression')
        if expected and result[0] != expected:
            self.fail(token, f'expected {expected}, got {result[0]}')
        return result

    def check(self):
        self.parse()
        for i, obj in enumerate(self.objects):
            if obj.center is None: continue
            target = obj.center
            if target.value == 'parent': obj.center = 'parent'; continue
            if target.value not in self.ids:
                self.fail(target, f'unknown anchor target {target.value!r}')
            anchor = self.ids[target.value]
            if self.objects[anchor].parent != obj.parent:
                self.fail(target, 'anchors may only reference the parent or a direct sibling')
            if anchor >= i:
                self.fail(target, 'anchor target must be declared before this item')
            obj.center = anchor
        for name, prop in self.properties.items():
            self.checked['state', name] = (*self.expression_code(prop.value, 0, prop.type), prop.token)
        for i, obj in enumerate(self.objects):
            for name, expr in obj.bindings.items():
                self.checked[i, name] = (*self.expression_code(expr, i, ATTRS[name][0]), expr.token)
            if obj.margins is not None:
                self.checked[i, 'anchors.margins'] = (*self.expression_code(obj.margins, i, 'int'),
                                                      obj.margins.token)
        visiting, visited = set(), set()

        def visit(key):
            if key in visiting: self.fail(self.checked[key][3], 'binding cycle detected')
            if key in visited: return
            visiting.add(key)
            for dependency in sorted(self.checked[key][2], key=str): visit(dependency)
            visiting.remove(key); visited.add(key); self.order.append(key)

        for key in self.checked: visit(key)
        # Validate handlers before generating any output.
        self.events = {}
        for i, obj in enumerate(self.objects):
            for statements in obj.handlers.values():
                code = []
                for token, names, operator, expression in statements:
                    key = self.reference(Expr(token, 'ref', names), i)
                    if key[0] != 'state' or self.properties[key[1]].readonly:
                        self.fail(token, 'handler target must be a writable component property')
                    prop = self.properties[key[1]]
                    _, value, _ = self.expression_code(expression, i, prop.type)
                    if operator != '=':
                        if prop.type != 'int': self.fail(token, 'compound assignment requires int')
                        function = 'add' if operator in ('+=', '++') else 'sub'
                        value = f'ame::ui::{function}(p_{key[1]}, {value})'
                    code.append(f'set_{key[1]}({value});')
                self.events[i] = code

    def emit(self):
        self.check()
        lines = ['#pragma once', '#include "ame/ui.h"',
                 'static_assert(ame::ui::kAbiVersion == 1, "regenerate QML component for this runtime");',
                 'namespace ame::qml {',
                 f'class {self.name} {{', '  bool bindings_dirty_ = true;', 'public:',
                 f'  ame::ui::Scene<{len(self.objects)}> scene;']
        for name, prop in self.properties.items():
            lines += ['private:', f'  {CPP[prop.type]} p_{name}{{}};',
                      f'  bool bound_{name} = true;', 'public:',
                      f'  {CPP[prop.type]} get_{name}() const {{ return p_{name}; }}']
            if not prop.readonly:
                lines += [f'  void set_{name}({CPP[prop.type]} value) {{',
                          f'    bound_{name} = false;',
                          f'    if (p_{name} != value) {{ p_{name} = value; bindings_dirty_ = true; }}', '  }']
        lines += [f'  {self.name}() {{']
        for i, obj in enumerate(self.objects):
            lines.append(f'    scene.add_node(ame::ui::Kind::{obj.kind}, {obj.parent});')
            if obj.fill: lines.append(f'    scene.set({i}, ame::ui::Property::Fill, 1);')
            if obj.center is not None:
                anchor = 'ame::ui::kAnchorParent' if obj.center == 'parent' else str(obj.center)
                lines.append(f'    scene.set({i}, ame::ui::Property::AnchorCenterIn, {anchor});')
        lines += ['    update();', '  }', '  void update() {', '    if (!bindings_dirty_) return;']
        for key in self.order:
            kind, code, deps, token = self.checked[key]
            lines.append(f'#line {token.line} {json.dumps(self.filename)}')
            if key[0] == 'state':
                lines.append(f'    if (bound_{key[1]}) p_{key[1]} = {code};')
            else:
                i, name = key
                if name == 'text': lines.append(f'    scene.set_text({i}, {code});')
                elif name == 'anchors.margins':
                    lines.append(f'    scene.set({i}, ame::ui::Property::AnchorMargins, {code});')
                else: lines.append(f'    scene.set({i}, ame::ui::Property::{ATTRS[name][1]}, static_cast<int32_t>({code}));')
        lines += ['    bindings_dirty_ = false;', '  }',
                  '  bool prepare(int32_t width, int32_t height) { update(); return scene.layout(width, height); }',
                  '  bool activate(int32_t target) {', '    switch (target) {']
        for i, code in self.events.items():
            lines += [f'    case {i}:']
            for statement in code:
                # A later statement may read a binding changed by an earlier
                # assignment. Evaluate before reads; layout/render stay batched.
                lines += ['      update();', '      ' + statement]
            lines += ['      update();', '      return true;']
        lines += ['    default: return false;', '    }', '  }',
                  '  void pointer_down(int32_t x, int32_t y) { scene.pointer_down(x, y); }',
                  '  bool pointer_up(int32_t x, int32_t y) { return activate(scene.pointer_up(x, y)); }',
                  '  bool activate_focus() { return activate(scene.activate_focus()); }',
                  '};', '} // namespace ame::qml', '']
        metadata = {'schema': 1, 'module': 'Ame.Ui', 'version': '1.0', 'component': self.name,
                    'source': self.filename, 'objects': [
                        {'index': i, 'type': obj.kind, 'id': obj.name, 'parent': obj.parent,
                         'line': obj.token.line, 'anchors': {
                             'fill': obj.fill,
                             'centerIn': 'parent' if obj.center == 'parent' else
                                         (self.objects[obj.center].name if obj.center is not None else None),
                             'margins': obj.margins is not None}} for i, obj in enumerate(self.objects)],
                    'properties': {name: {'type': p.type, 'readonly': p.readonly}
                                   for name, p in self.properties.items()},
                    'bindings': [
                        {'target': list(key), 'type': self.checked[key][0],
                         'dependencies': [list(dependency) for dependency in
                                          sorted(self.checked[key][2], key=str)],
                         'line': self.checked[key][3].line}
                        for key in self.order]}
        return '\n'.join(lines), metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--header', required=True, type=Path)
    parser.add_argument('--metadata', required=True, type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Z][A-Za-z0-9_]*', args.source.stem):
        parser.error('component filename must start with an uppercase ASCII letter')
    try:
        try:
            filename = args.source.resolve().relative_to(Path(__file__).resolve().parents[2]).as_posix()
        except ValueError:
            filename = args.source.as_posix()
        header, metadata = Compiler(args.source.read_text(), filename, args.source.stem).emit()
    except (Diagnostic, OSError) as error:
        print(error, file=sys.stderr); return 1
    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.metadata.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(header)
    args.metadata.write_text(json.dumps(metadata, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
