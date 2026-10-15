-- Operator 2026-10-15: character names allow 2 to 20 display columns and a CJK
-- character counts as two columns, so a UTF-16 name needs up to 40 octets.
-- The 2-octet floor and the even-length rule stay unchanged.
ALTER TABLE Characters DROP CHECK CharactersNameLength;
ALTER TABLE Characters ADD CONSTRAINT CharactersNameLength CHECK (OCTET_LENGTH(NameUtf16) BETWEEN 2 AND 40 AND MOD(OCTET_LENGTH(NameUtf16), 2) = 0);
