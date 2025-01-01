BEGIN;

SET client_min_messages = warning;
SET client_encoding = 'UTF8';

CREATE EXTENSION IF NOT EXISTS pgcrypto;

CREATE TYPE account_status AS ENUM (
    'provisioned',
    'active',
    'suspended'
);

CREATE TABLE accounts (
    id                  BIGSERIAL PRIMARY KEY,
    email               VARCHAR(254) UNIQUE NOT NULL,
    status              account_status DEFAULT 'provisioned' NOT NULL,
    login               VARCHAR(254) UNIQUE NOT NULL,
    created_at          INTEGER DEFAULT EXTRACT(EPOCH FROM NOW()) NOT NULL,
    status_changed_at   INTEGER,
    activated_at        INTEGER,
    suspended_at        INTEGER,
    unsuspended_at      INTEGER
);

CREATE INDEX accounts_id_status_idx ON accounts (id, status);

CREATE TYPE token_action AS ENUM (
    'activation',
    'password_recovery'
);

CREATE TABLE tokens (
    id          BIGSERIAL PRIMARY KEY,
    action      token_action NOT NULL,
    secret      BYTEA DEFAULT gen_random_bytes(32) UNIQUE NOT NULL,
    code        VARCHAR(5) DEFAULT LPAD(TO_CHAR(RANDOM() * 100000, 'FM00000'), 5, '0'),
    account     BIGINT NOT NULL,
    expires_at  INTEGER DEFAULT EXTRACT(EPOCH FROM NOW() + INTERVAL '15 minute') NOT NULL,
    consumed_at INTEGER,
    created_at  INTEGER DEFAULT EXTRACT(EPOCH FROM NOW()) NOT NULL,

    FOREIGN KEY (account) REFERENCES accounts (id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED
);

CREATE INDEX tokens_account_fkey ON tokens (account);

CREATE INDEX tokens_id_expires_consumed_action_idx ON tokens (id, expires_at, consumed_at, action);

CREATE TYPE job_type AS ENUM (
    'mailroom'
);

-- Job queue cursor table to track the last processed token
CREATE TABLE jobs (
    job_type job_type PRIMARY KEY,
    last_seq BIGINT
);

CREATE INDEX jobs_last_seq_type_idx ON jobs (last_seq, job_type);

-- Initialize the user action queue with a default value
INSERT INTO
jobs
    (last_seq, job_type)
VALUES
    (0, 'mailroom');

-- Triggers

CREATE OR REPLACE FUNCTION trg_before_account_insert()
RETURNS TRIGGER AS $$
BEGIN
    IF (NEW.status = 'provisioned') THEN
        INSERT INTO tokens (account, action)
        VALUES (NEW.id, 'activation');
    END IF;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER before_account_insert
    BEFORE INSERT ON accounts
    FOR EACH ROW
    EXECUTE FUNCTION trg_before_account_insert ();

CREATE OR REPLACE FUNCTION trg_before_account_status_change ()
    RETURNS TRIGGER
    AS $$
DECLARE
    ts integer := extract(epoch FROM now());
BEGIN
    IF (NEW.status = OLD.status) THEN
        RETURN NEW;
    END IF;
    NEW.status_changed_at = ts;
    IF (NEW.status = 'active') THEN
        IF (OLD.status = 'provisioned') THEN
            NEW.activated_at = ts;
        ELSIF (OLD.status = 'suspended') THEN
            NEW.unsuspended_at = ts;
            NEW.suspended_at = NULL;
            IF (OLD.activated_at IS NULL) THEN
              NEW.status = 'provisioned';
            END IF;
        END IF;
    ELSIF (NEW.status = 'suspended') THEN
        NEW.suspended_at = ts;
        NEW.unsuspended_at = NULL;
    END IF;
    RETURN new;
END;
$$
LANGUAGE plpgsql;

CREATE TRIGGER before_account_status_change
    BEFORE UPDATE OF status ON accounts
    FOR EACH ROW
    EXECUTE FUNCTION trg_before_account_status_change ();

CREATE OR REPLACE FUNCTION trg_after_token_consumed ()
    RETURNS TRIGGER
    AS $$
BEGIN
    IF (NEW.action != 'activation') THEN
        RETURN NULL;
    END IF;
    UPDATE
        accounts
    SET
        status = 'active'
    WHERE
        id = NEW.account
        AND status = 'provisioned';
    RETURN NULL;
END;
$$
LANGUAGE plpgsql;

CREATE TRIGGER after_token_consumed
    AFTER UPDATE OF consumed_at ON tokens
    FOR EACH ROW
    WHEN (NEW.consumed_at IS NOT NULL AND OLD.consumed_at IS NULL)
    EXECUTE FUNCTION trg_after_token_consumed ();

CREATE OR REPLACE FUNCTION trg_after_token_inserted()
    RETURNS TRIGGER
    LANGUAGE plpgsql
AS $$
BEGIN
    NOTIFY token_insert;
    RETURN NULL;
END;
$$;

CREATE TRIGGER after_token_inserted
    AFTER INSERT ON tokens
    FOR EACH ROW
    EXECUTE FUNCTION trg_after_token_inserted ();

END;
