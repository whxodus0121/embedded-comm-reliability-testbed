import pathlib
import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "common"))
from runner import main

if __name__ == "__main__":
    main(4)
