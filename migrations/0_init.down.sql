BEGIN;

DROP INDEX IF EXISTS accounts_id_status_idx;
DROP INDEX IF EXISTS tokens_account_fkey;
DROP INDEX IF EXISTS tokens_id_expires_consumed_action_idx;
DROP INDEX IF EXISTS jobs_last_seq_type_idx;

DROP TRIGGER IF EXISTS before_account_insert ON accounts;
DROP TRIGGER IF EXISTS before_account_status_change ON accounts;
DROP TRIGGER IF EXISTS after_token_consumed ON tokens;
DROP TRIGGER IF EXISTS after_token_inserted ON tokens;

DROP FUNCTION IF EXISTS trg_before_account_insert();
DROP FUNCTION IF EXISTS trg_before_account_status_change();
DROP FUNCTION IF EXISTS trg_after_token_consumed();
DROP FUNCTION IF EXISTS trg_after_token_inserted();

DROP TABLE IF EXISTS jobs;
DROP TABLE IF EXISTS tokens;
DROP TABLE IF EXISTS accounts;

DROP TYPE IF EXISTS job_type;
DROP TYPE IF EXISTS token_action;
DROP TYPE IF EXISTS account_status;

DROP EXTENSION IF EXISTS pgcrypto;

END;
