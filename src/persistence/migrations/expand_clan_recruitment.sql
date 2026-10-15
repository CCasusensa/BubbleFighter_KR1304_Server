-- Thirteenth additive migration. Preserve existing 0/1 rows and default 0.
-- Native recruitment modes: 0 stopped, 1 manual, 2 automatic.
ALTER TABLE Clans
    DROP CHECK CK_Clans_Recruiting,
    ADD CONSTRAINT CK_Clans_RecruitmentMode CHECK (IsRecruiting IN (0, 1, 2));
