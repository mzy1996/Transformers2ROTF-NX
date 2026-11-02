import subprocess, sys
r = subprocess.run(['ffmpeg', '-encoders'], capture_output=True, text=True)
for line in r.stdout.splitlines():
    if 'vc1' in line.lower():
        print(line)
print('stderr:', r.stderr[:500], file=sys.stderr)
