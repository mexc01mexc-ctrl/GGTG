import sys, os, glob, subprocess

def main():
    libs_dir = os.environ.get('LIBS_DIR')
    if not libs_dir:
        print('[ERROR] LIBS_DIR environment variable is not set.')
        return 1
    qt_version = sys.argv[1] if len(sys.argv) > 1 else '5.15.19'
    patch_dir = os.path.join(libs_dir, 'patches', f'qtbase_{qt_version}')
    if not os.path.isdir(patch_dir):
        print(f'[ERROR] Patch directory not found: {patch_dir}')
        return 1
    patches = sorted(glob.glob(os.path.join(patch_dir, '*.patch')))
    if not patches:
        print(f'[WARN] No patches found in {patch_dir}')
        return 0
    print(f'Applying {len(patches)} patches from {patch_dir}...')
    for p in patches:
        patch_name = os.path.basename(p)
        print(f'  -> Applying {patch_name} ...', flush=True)
        res = subprocess.run(['git', 'apply', p, '-v'])
        if res.returncode != 0:
            print(f'[ERROR] Failed to apply {patch_name} with exit code {res.returncode}')
            return res.returncode
    print('All patches applied successfully.')
    return 0

if __name__ == '__main__':
    sys.exit(main())
