# Renaming Repository to SmallestGraphicEngine

Your Git repository has been successfully initialized! To complete the rename to "SmallestGraphicEngine", follow these steps:

## Option 1: Rename the Directory (After closing the running application)

1. **Close the GraphicEngine.exe** if it's still running (press ESC in the window)

2. **Navigate to parent directory and rename**:
   ```powershell
   cd C:\Users\Bas\Source\Repos
   Rename-Item -Path GraphicEngine -NewName SmallestGraphicEngine
   cd SmallestGraphicEngine
   ```

3. **Verify Git repository is intact**:
   ```powershell
   git status
   git log --oneline
   ```

## Option 2: Create GitHub Repository

1. **Go to GitHub** and create a new repository named `SmallestGraphicEngine`

2. **Add remote and push**:
   ```powershell
   git remote add origin https://github.com/YOUR_USERNAME/SmallestGraphicEngine.git
   git branch -M main
   git push -u origin main
   ```

## Option 3: Copy to New Location

If you want to keep both, copy the entire directory:

```powershell
cd C:\Users\Bas\Source\Repos
Copy-Item -Path GraphicEngine -Destination SmallestGraphicEngine -Recurse
cd SmallestGraphicEngine
git status  # Verify the repository is intact
```

## What Was Committed

✅ All source files:
- `src/main.cpp`, `src/Shader.h`, `src/Camera.h`
- `shaders/*.vert`, `shaders/*.frag`

✅ Build configuration:
- `CMakeLists.txt`
- `build.ps1`

✅ Documentation:
- `README.md` - Quick start guide
- `TUTORIAL.md` - Complete educational guide

✅ Git configuration:
- `.gitignore` - Excludes build artifacts, IDE files

## Repository Info

- **Branch**: master
- **Commit**: d04e1dd
- **Files**: 12 tracked files
- **Size**: ~1,200 lines of code

## Next Steps

After renaming, you can:
- Push to GitHub for backup and sharing
- Continue developing new features
- Tag releases: `git tag -a v1.0 -m "First release"`

---

**Note**: The directory rename failed because GraphicEngine.exe is still running. Close it first, then rename.
