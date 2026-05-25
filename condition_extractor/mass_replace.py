import os
import re

targets_dir = '/home/mashmallow/source/liberator/targets'
analysis_files = []
for root, dirs, files in os.walk(targets_dir):
    for file in files:
        if file == 'analysis.sh':
            analysis_files.append(os.path.join(root, file))

for f in analysis_files:
    with open(f, 'r') as file:
        content = file.read()
    
    if '/condition_extractor/bin/extractor' in content:
        # Check if we already injected PROF_EXTRACTOR into this file
        if '$PROF_EXTRACTOR' not in content:
            # First, simply replace the command with an environment variable prefixed command
            new_content = re.sub(
                r'(perf record -g --call-graph dwarf -F 99 )?("?\$TOOLS_DIR"?/condition_extractor/bin/extractor)',
                r'$PROF_EXTRACTOR \2',
                content
            )
            if new_content != content:
                with open(f, 'w') as file:
                   file.write(new_content)
                print(f"Updated {f}")
            else:
                print(f"Could not update {f} - regex match failed")
        else:
            print(f"Already updated {f}")