import subprocess, os
folder = r'J:/XBOX360 port/Transformers Revenge of the Fallen/game/movies'
for f in sorted(os.listdir(folder)):
    if f.lower().endswith('.wmv'):
        path = os.path.join(folder, f)
        r = subprocess.run(['ffprobe', '-v', 'error', '-show_streams', '-of', 'json', path], capture_output=True, text=True)
        import json
        info = json.loads(r.stdout)
        vtag = 'none'
        atag = 'none'
        for s in info.get('streams', []):
            if s.get('codec_type') == 'video':
                vtag = s.get('codec_tag_string', 'unknown')
            elif s.get('codec_type') == 'audio':
                atag = s.get('codec_tag_string', 'unknown')
        print(f'{f}: video={vtag} audio={atag}')
