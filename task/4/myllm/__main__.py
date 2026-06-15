"""允许 python -m myllm 执行"""
import sys
from .main import main

sys.exit(main())
