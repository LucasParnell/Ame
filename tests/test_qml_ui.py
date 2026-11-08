from pathlib import Path
import importlib.util
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ame_qmlc', ROOT / 'scripts/ui/qmlc.py')
qmlc = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = qmlc
spec.loader.exec_module(qmlc)


class QmlUI(unittest.TestCase):
    def compile_source(self, body):
        return qmlc.Compiler('import Ame.Ui 1.0\n' + body, 'Test.qml', 'Test').emit()

    def rejects(self, body, message):
        with self.assertRaisesRegex(qmlc.Diagnostic, r'Test.qml:\d+:\d+: error: .*' + message):
            self.compile_source(body)

    def test_source_diagnostics(self):
        self.rejects('Item { width: missing }', 'unknown name')
        self.rejects('Item { width: true }', 'expected int')
        self.rejects('Item { width: 1; width: 2 }', 'duplicate binding')
        self.rejects('Item { Text { id: a } Text { id: a } }', 'duplicate or reserved')
        self.rejects('Item { property int a: b; property int b: a }', 'binding cycle')
        self.rejects('Item { width: height; height: width }', 'binding cycle')
        self.rejects('Item { color: "blue" }', 'color must')
        self.rejects('Item { width: 2147483648 }', 'int32 range')
        self.rejects('Item { anchors.fill: parent }', 'root has no parent')
        self.rejects('Item { Button { anchors.fill: parent; width: 2 } }', 'cannot be combined')
        self.rejects('Item { text: "wrong type" }', 'text belongs')
        self.rejects('Item { property int n: 0; Button { onClicked: { n = false; } } }', 'expected int')
        self.rejects('Item { readonly property int n: 0; Button { onClicked: { n++; } } }', 'writable')
        self.rejects('Item { width: eval("unsafe") }', 'only the pure')
        self.rejects('Item { Rectangle { width: parent.width } }', 'implicit/default')
        self.rejects('Item { width: ' + '(' * 65 + '1' + ')' * 65 + ' }', 'nesting levels')

    def test_typed_ir_and_dependency_order(self):
        header, metadata = self.compile_source('''Item {
            id: root
            readonly property int doubled: count * 2
            property int count: 3
            property string label: "Count " + count
            width: 20
            height: 30
            Text { text: root.label }
        }''')
        self.assertLess(header.index('p_count = 3'), header.index('p_doubled = ame::ui::mul'))
        self.assertEqual(metadata['properties']['doubled'], {'type': 'int', 'readonly': True})
        self.assertIn('#line', header)
        self.assertEqual(metadata['objects'][1]['parent'], 0)

    def test_anchor_conflicts(self):
        self.rejects('Item { Button { anchors.centerIn: parent; x: 1 } }', 'centerIn cannot be combined with x')
        self.rejects('Item { Button { anchors.centerIn: parent; y: 1 } }', 'centerIn cannot be combined with y')
        self.rejects('Item { Button { anchors.fill: parent; height: 2 } }', 'cannot be combined with height')
        self.rejects('Item { Button { anchors.margins: 4 } }', 'requires anchors.fill or anchors.centerIn')
        self.rejects('Item { Button { anchors.fill: parent; anchors.centerIn: parent } }', 'conflicting anchor')

    def test_anchor_targets(self):
        self.rejects('Item { Button { anchors.centerIn: missing } }', 'unknown anchor target')
        self.rejects('Item { Column { Item { id: inner } } Button { anchors.centerIn: inner } }',
                     'parent or a direct sibling')
        self.rejects('Item { Button { anchors.centerIn: later } Button { id: later } }',
                     'declared before this item')

    def build_probe(self, body, probe):
        header, metadata = self.compile_source(body)
        with tempfile.TemporaryDirectory(prefix='ame-qml-anchor-') as work:
            work = Path(work)
            (work / 'Test.h').write_text(header)
            (work / 'probe.cc').write_text(probe)
            result = subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I' + str(ROOT / 'userland/libui/include'),
                '-I' + str(ROOT / 'shared/include'), str(work / 'probe.cc'), '-o', str(work / 'probe')],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(work / 'probe')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return metadata

    def test_anchors_center_and_margins(self):
        metadata = self.build_probe('''Item {
            id: root
            width: 200
            height: 100
            Rectangle { id: marker; width: 40; height: 20 }
            Rectangle { id: centred; anchors.centerIn: parent; width: 20; height: 10 }
            Rectangle { id: around; anchors.centerIn: marker; anchors.margins: 4; width: 10; height: 10 }
            Rectangle { id: padded; anchors.fill: parent; anchors.margins: 5 }
        }''', '''#include "Test.h"
            #include <cassert>
            int main() {
                ame::qml::Test app;
                assert(app.prepare(200, 100));
                const auto centred = app.scene.geometry(2).box;
                assert(centred.x == 90 && centred.y == 45 && centred.width == 20 && centred.height == 10);
                const auto around = app.scene.geometry(3).box;
                assert(around.x == 11 && around.y == 1 && around.width == 18 && around.height == 18);
                const auto padded = app.scene.geometry(4).box;
                assert(padded.x == 5 && padded.y == 5 && padded.width == 190 && padded.height == 90);
            }''')
        self.assertEqual(metadata['objects'][2]['anchors']['centerIn'], 'parent')
        self.assertEqual(metadata['objects'][3]['anchors']['centerIn'], 'marker')
        self.assertTrue(metadata['objects'][3]['anchors']['margins'])

    def test_z_paint_order(self):
        self.build_probe('''Item {
            width: 100
            height: 100
            Rectangle { id: a; width: 50; height: 50; color: "#ff0000" }
            Rectangle { id: b; width: 50; height: 50; color: "#00ff00"; z: 1 }
            Rectangle { id: c; width: 10; height: 10; color: "#0000ff" }
            Rectangle { id: d; width: 5; height: 5; color: "#ffffff"; z: -5 }
        }''', '''#include "Test.h"
            #include <cassert>
            #include <cstdio>
            struct Sink {
                uint32_t seen[16]{}; int count = 0;
                bool rectangle(ame::ui::Rect, ame::ui::Rect, uint32_t color) {
                    if (color != 0) seen[count++] = color; return true;
                }
                bool text(ame::ui::Rect, ame::ui::Rect, const ame::ui::Text&, uint32_t) { return true; }
            };
            int main() {
                ame::qml::Test app;
                assert(app.prepare(100, 100));
                Sink sink;
                assert(app.scene.draw(sink));
                // Lower z first, then declaration order among equal z, then higher z.
                assert(sink.count == 4);
                assert(sink.seen[0] == 0xffffffffu && sink.seen[1] == 0xffff0000u);
                assert(sink.seen[2] == 0xff0000ffu && sink.seen[3] == 0xff00ff00u);
            }''')

    def test_z_is_depth_first_within_a_sibling(self):
        self.build_probe('''Item {
            width: 100
            height: 100
            Rectangle { id: outer; width: 50; height: 50; color: "#ff0000"
                Rectangle { width: 10; height: 10; color: "#0000ff" }
            }
            Rectangle { id: later; width: 50; height: 50; color: "#00ff00"; z: 5 }
        }''', '''#include "Test.h"
            #include <cassert>
            struct Sink {
                uint32_t seen[8]{}; int count = 0;
                bool rectangle(ame::ui::Rect, ame::ui::Rect, uint32_t color) { seen[count++] = color; return true; }
                bool text(ame::ui::Rect, ame::ui::Rect, const ame::ui::Text&, uint32_t) { return true; }
            };
            int main() {
                ame::qml::Test app;
                assert(app.prepare(100, 100));
                Sink sink;
                assert(app.scene.draw(sink));
                // The child belongs to `outer`, so it precedes the later sibling
                // even though that sibling has a far higher z.
                assert(sink.count == 3);
                assert(sink.seen[0] == 0xffff0000u && sink.seen[1] == 0xff0000ffu);
                assert(sink.seen[2] == 0xff00ff00u);
            }''')

    def test_z_hit_order(self):
        self.build_probe('''Item {
            width: 100
            height: 100
            Button { id: under; width: 50; height: 50; z: 0 }
            Button { id: over; width: 50; height: 50; z: 2 }
            Button { id: above; width: 50; height: 50 }
        }''', '''#include "Test.h"
            #include <cassert>
            int main() {
                ame::qml::Test app;
                assert(app.prepare(100, 100));
                // over (z 2) wins the shared area; above (z 0) is only on top of under.
                assert(app.scene.hit_test(10, 10) == 2);
                app.scene.set(2, ame::ui::Property::Z, -1);
                assert(app.prepare(100, 100));
                assert(app.scene.hit_test(10, 10) == 3);
            }''')

    def test_native_runtime_and_generated_counter(self):
        with tempfile.TemporaryDirectory(prefix='ame-qml-') as work:
            work = Path(work)
            source = ROOT / 'userland/app/ui-demo/Counter.qml'
            subprocess.run([sys.executable, str(ROOT / 'scripts/ui/qmlc.py'), str(source),
                            '--header', str(work / 'Counter.h'), '--metadata', str(work / 'Counter.json')],
                           check=True, capture_output=True)
            common = ['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                      '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                      '-I' + str(ROOT / 'userland/libui/include'),
                      '-I' + str(ROOT / 'shared/include'), '-I' + str(work)]
            artifacts = ROOT / 'out/ui'
            artifacts.mkdir(parents=True, exist_ok=True)
            for name in ('ui_runtime_probe', 'qml_counter_probe'):
                result = subprocess.run(common + [str(ROOT / 'tests' / (name + '.cc')),
                                                 '-o', str(work / name)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

                command = [str(work / name)]
                if name == 'qml_counter_probe':
                    command += [str(artifacts / 'counter-before.ppm'), str(artifacts / 'counter-after.ppm')]
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_handler_reads_fresh_bindings_and_assignment_removes_binding(self):
        source = '''Item {
            property int count: 1
            property int derived: count * 3
            property int captured: 0
            Button { width: 20; height: 20; onClicked: { count = 2; captured = derived; } }
        }'''
        header, _ = self.compile_source(source)
        with tempfile.TemporaryDirectory(prefix='ame-qml-binding-') as work:
            work = Path(work)
            (work / 'Test.h').write_text(header)
            (work / 'probe.cc').write_text('''#include "Test.h"
                #include <cassert>
                int main() {
                    ame::qml::Test app;
                    assert(app.get_derived() == 3);
                    assert(app.activate(1));
                    assert(app.get_captured() == 6);
                    app.set_derived(9); app.set_count(4); app.update();
                    assert(app.get_derived() == 9);
                }''')
            result = subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I' + str(ROOT / 'userland/libui/include'),
                str(work / 'probe.cc'), '-o', str(work / 'probe')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(work / 'probe')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
