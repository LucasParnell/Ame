#!/usr/bin/env python3
import os
import pathlib
import sys

src = pathlib.Path(os.environ.get('DANGO_LINUX_SRC', sys.argv[1] if len(sys.argv) > 1 else ''))
if not src or not src.exists():
    print(f"error: Linux source not found: {src}", file=sys.stderr)
    raise SystemExit(1)

smp = src / 'arch/arm64/kernel/smp.c'
if smp.exists():
    text = smp.read_text()
    if 'dango-trace smp_prepare_boot_cpu: before cpuinfo_store_boot_cpu' not in text:
        text = text.replace(
            'void __init smp_prepare_boot_cpu(void)\n{',
            'void __init smp_prepare_boot_cpu(void)\n{\n\tpr_info("dango-trace smp_prepare_boot_cpu: entry\\n");',
            1,
        )
        text = text.replace(
            '\tcpuinfo_store_boot_cpu();',
            '\tpr_info("dango-trace smp_prepare_boot_cpu: before cpuinfo_store_boot_cpu\\n");\n\tcpuinfo_store_boot_cpu();\n\tpr_info("dango-trace smp_prepare_boot_cpu: after cpuinfo_store_boot_cpu\\n");',
            1,
        )
        text = text.replace(
            '\tsetup_boot_cpu_features();',
            '\tpr_info("dango-trace smp_prepare_boot_cpu: before setup_boot_cpu_features\\n");\n\tsetup_boot_cpu_features();\n\tpr_info("dango-trace smp_prepare_boot_cpu: after setup_boot_cpu_features\\n");',
            1,
        )
        smp.write_text(text)
        print('dango linux trace: patched arch/arm64/kernel/smp.c')
    else:
        print('dango linux trace: arch/arm64/kernel/smp.c already patched')
else:
    print('warning: arch/arm64/kernel/smp.c not found; skipping trace patch', file=sys.stderr)

main = src / 'init/main.c'
if main.exists():
    text = main.read_text()
    if 'dango-trace start_kernel: entry' not in text:
        text = text.replace(
            'asmlinkage __visible __init __no_sanitize_address __noreturn __no_stack_protector\nvoid start_kernel(void)\n{',
            'asmlinkage __visible __init __no_sanitize_address __noreturn __no_stack_protector\nvoid start_kernel(void)\n{\n\tpr_info("dango-trace start_kernel: entry\\n");',
            1,
        )
        # These replacements are deliberately best-effort; if upstream reshuffles
        # start_kernel, the smp.c trace is still the important one for this hang.
        for needle, marker in [
            ('setup_command_line(command_line);', 'before setup_command_line'),
            ('setup_nr_cpu_ids();', 'before setup_nr_cpu_ids'),
            ('setup_per_cpu_areas();', 'before setup_per_cpu_areas'),
            ('smp_prepare_boot_cpu();', 'before smp_prepare_boot_cpu'),
            ('boot_cpu_hotplug_init();', 'before boot_cpu_hotplug_init'),
            ('build_all_zonelists(NULL);', 'before build_all_zonelists'),
            ('page_alloc_init();', 'before page_alloc_init'),
            ('trap_init();', 'before trap_init'),
            ('mm_core_init();', 'before mm_core_init'),
            ('sched_init();', 'before sched_init'),
            ('init_IRQ();', 'before init_IRQ'),
            ('time_init();', 'before time_init'),
            ('rest_init();', 'before rest_init'),
        ]:
            text = text.replace(needle, f'pr_info("dango-trace start_kernel: {marker}\\n");\n\t{needle}', 1)
        main.write_text(text)
        print('dango linux trace: patched init/main.c')
    else:
        print('dango linux trace: init/main.c already patched')
