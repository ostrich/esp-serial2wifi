"""Locate native harness boundaries without depending on C whitespace layout."""

import re


class SourceText(str):
    def __getitem__(self, key):
        value = super().__getitem__(key)
        return SourceText(value) if isinstance(key, slice) else value

    def index(self, marker, start=0, end=None):
        stop = len(self) if end is None else end
        try:
            return super().index(marker, start, stop)
        except ValueError:
            tokens = re.findall(r"\w+|[^\w\s]", marker)
            if not tokens:
                raise
            match = re.search(r"\s*".join(map(re.escape, tokens)), self[start:stop])
            if not match:
                raise ValueError(f"Native source marker not found: {marker!r}") from None
            return start + match.start()
