"""Public native command tokens must survive the early unknown-command gate."""
import re,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]/'esp32_marauder'
class CommandRegistryTests(unittest.TestCase):
 def test_all_native_tokens_are_dispatched_or_explicitly_deprecated(self):
  header=(ROOT/'CommandLine.h').read_text();source=(ROOT/'CommandLine.cpp').read_text()
  registry=source[source.index('{"channel",'):source.index('const char* usageFor')]
  registered=set(re.findall(r'\{"([a-z][a-z0-9_]*)"\s*,',registry))
  tokens={name for symbol,name in re.findall(r'const char PROGMEM (\w*CMD\w*)\[\] = "([a-z][a-z0-9_]*)";',header) if not symbol.startswith('HELP')}
  self.assertEqual(tokens-registered,{'btwardrive'})
  self.assertIn('name=="btwardrive"',source)
if __name__=='__main__':unittest.main()
