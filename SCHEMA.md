# Schema

## Accounts table

The `accounts` table manages user data and tracks account lifecycle states.

```sql
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
```

Here, the `status` field tracks the current state of the account (`provisioned`, `active`, or `suspended`), while timestamps like `status_changed_at` and `activated_at` capture important lifecycle events, helping to maintain the `status` field correctly during transitions and ensuring accurate tracking of account states over time.

---

## Tokens table

The `tokens` table tracks actionable tokens, such as those used for activation or password recovery.

```sql
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
```

##### Key columns:

- `action` – Specifies the token type (`activation` or `password recovery`).
- `secret` – A unique and secure token string.
- `code` – A short, human-readable security code.
- `expires_at` – Defines the expiration time for tokens, defaulting to 15 minutes.

This table complements the `accounts` table by managing token-based actions, with relationships maintained through the foreign key `account`.

---

## Triggers

PostgreSQL triggers allow us to automate processes in response to data changes. Below are the triggers to ensure seamless management of account status transitions, token consumption, and notifications.

### 1. **Before account insert**

- **Event**: Before an account is inserted into the `accounts` table.
- **Purpose**: Automatically creates an activation token when a new account is provisioned.

```plpgsql
CREATE OR REPLACE FUNCTION trg_before_account_insert()
RETURNS TRIGGER AS $$
BEGIN
    IF (NEW.status = 'provisioned') THEN
        INSERT INTO
        tokens
            (account, action)
        VALUES
            (NEW.id, 'activation');
    END IF;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER before_account_insert
    BEFORE INSERT ON accounts
    FOR EACH ROW
    EXECUTE FUNCTION trg_before_account_insert ();
```

> ##### Why not an AFTER trigger?
>
> While it may seem logical to create the token _after_ confirming the account's existence (since the token is ultimately tied to the account), this approach has a critical flaw: if the token insertion fails, we could end up with an account that lacks a corresponding activation token, which would break downstream processes.
>
> The `BEFORE` trigger ensures that token creation and account insertion are part of the same transaction, guaranteeing the consistency we need. If token creation fails, the entire transaction will be rolled back, preventing the system from entering an invalid state.
>
> This is why the `DEFERRABLE INITIALLY DEFERRED` constraint is applied to the `tokens` table. It allows a token to be inserted even before the associated account is created, provided both operations occur within the same transaction.

### 2. **Before account status change**

- **Event**: Before an account's `status` is updated.
- **Purpose**: Updates timestamps for key status changes (e.g., activated, suspended, unsuspended).

```plpgsql
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
            -- Revert status to 'provisioned' if never activated
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
```

### 3. **After token consumed**

- **Event**: After a token's `consumed_at` field in `tokens` is updated.
- **Purpose**: Activates the associated account when an activation token is consumed.

```plpgsql
CREATE OR REPLACE FUNCTION trg_after_token_consumed ()
    RETURNS TRIGGER
    AS $$
BEGIN
    IF (NEW.action != 'activation') THEN
        RETURN NULL;
    END IF;
    -- Activate account
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
```

### 4. **After token inserted**

- **Event**: After a token is inserted into the `tokens` table.
- **Purpose**: Notifies external services that a new token has been created.

```plpgsql
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
```

---

## Let's try it out!

Follow these steps to test the triggers and notifications in action:

### Setting your environment

_(Skip this section if you've already set up the tables and triggers.)_

Clone the [`tetsuo/mailroom`](https://github.com/tetsuo/mailroom) repository:

```sh
git clone https://github.com/tetsuo/mailroom.git
```

Run the following command to create a new database in PostgreSQL:

```sh
createdb mailroom
```

Then, navigate to the `migrations` folder and run:

```sh
psql -d mailroom < 0_init.up.sql
```

Alternatively, you can use [go-migrate](https://github.com/golang-migrate/) which is often my preference.

---

### Inspect the initial state

Before adding any data, let's take a look at the initial state of the `jobs` table:

```sh
psql -d mailroom -c "SELECT * FROM jobs;"
```

You should see one row with `job_type` set to `mailroom` and `last_seq` set to zero:

```
 job_type | last_seq
----------+----------
 mailroom |        0
(1 row)
```

---

### Create a new account

Insert a new account into the `accounts` table. This should automatically generate an activation token.

```sql
INSERT INTO accounts (email, login)
	VALUES ('user@example.com', 'user123');
```

**Tip:** To insert three records with randomized email and login fields, use the following command:

```sh
printf "%.0sINSERT INTO accounts (email, login) VALUES ('user' || md5(random()::text) || '@fake.mail', 'user' || substr(md5(random()::text), 1, 20));\n" {1..3} | \
    psql -d mailroom
```

**Expected outcome**:

- A new account with `status = 'provisioned'` is added to `accounts`.
- An activation token is automatically inserted into the `tokens` table, linked to the account.

Verify:

```sql
SELECT * FROM accounts WHERE id = 1;
SELECT * FROM tokens WHERE account = 1;
```

Here's an example `account` record:

```
-[ ACCOUNT 1 ]-------------------------------------------------------------------
id                | 1
email             | usere3213152e8cdf722466a011b1eaa3c98@fake.mail
status            | provisioned
login             | user85341405cb33cbe89a5f
created_at        | 1735709763
status_changed_at |
activated_at      |
suspended_at      |
unsuspended_at    |
```

The corresponding `token` record generated by the trigger function:

```
-[ TOKEN 1 ]---------------------------------------------------------------------
id          | 1
action      | activation
secret      | \x144d3ba23d4e60f80d3cb5cf25783539ba267af34aecd71d7cc888643c912fb7
code        | 06435
account     | 1
expires_at  | 1735710663
consumed_at |
created_at  | 1735709763
```

---

### Consume the activation token

Simulate token consumption by updating the `consumed_at` field in the `tokens` table.

```sql
UPDATE
	tokens
SET
	consumed_at = extract(epoch FROM now())
WHERE
	account = 1
	AND action = 'activation';
```

**Expected outcome**:

- The account's `status` in `accounts` should change to `active`.
- The `activated_at` timestamp should be updated in `accounts`.

Verify:

```sql
SELECT * FROM accounts WHERE id = 1;
SELECT * FROM tokens WHERE account = 1;
```

---

### Suspend the account

Change the account's status to `suspended` to test the suspension flow.

```sql
UPDATE accounts SET status = 'suspended' WHERE id = 1;
```

**Expected outcome**:

- The account's `suspended_at` timestamp is updated.
- The `unsuspended_at` field is cleared.

Verify:

```sql
SELECT * FROM accounts WHERE id = 1;
```

---

### Unsuspend the account

Restore the account's status to `active`.

```sql
UPDATE accounts SET status = 'active' WHERE id = 1;
```

**Expected outcome**:

- The account's `unsuspended_at` timestamp is updated.
- The `suspended_at` field is cleared.

Verify:

```sql
SELECT * FROM accounts WHERE id = 1;
```

---

### Observe notifications

Listen for token creation notifications on the `token_insert` channel using `LISTEN`:

```sql
LISTEN token_insert;
```

Next, insert some dummy data into the `accounts` table (or directly into `tokens`).

**Expected outcome**:

The `LISTEN` session should immediately display a notification like:

```
Asynchronous notification "token_insert" with payload "" received.
```

`psql` might need a little nudge (empty `;`) to display notifications:

```
mailroom=# LISTEN token_insert;
LISTEN
mailroom=# ;
Asynchronous notification "token_insert" received from server process with PID 5148.
Asynchronous notification "token_insert" received from server process with PID 5148.
Asynchronous notification "token_insert" received from server process with PID 5148.
```

_These notifications signal that new tokens have arrived—it's time to start processing them._

---

## Jobs table

Now we need to build a mechanism to collect newly added tokens. To do that, we'll define a query that manages their progression through the queue.

We use the jobs table to maintain a cursor that advances through tokens. This table simply tracks the last processed token (`last_seq`) for each job type:

```sql
CREATE TYPE job_type AS ENUM (
    'mailroom'
);

CREATE TABLE jobs (
    job_type job_type PRIMARY KEY,
    last_seq BIGINT
);
```

**Initialize the mailroom queue:**

```sql
INSERT INTO
jobs
    (last_seq, job_type)
VALUES
    (0, 'mailroom');
```

### Retrieving pending jobs

The following query retrieves relevant job data (tokens and account details), ensuring only valid, unexpired, and unprocessed tokens are selected, with accounts in the correct status for the intended action.

```sql
SELECT
    t.account,
    t.secret,
    t.code,
    t.expires_at,
    t.id,
    t.action,
    a.email,
    a.login
FROM
    jobs
    JOIN tokens t
        ON t.id > jobs.last_seq
        AND t.expires_at > EXTRACT(EPOCH FROM NOW())
        AND t.consumed_at IS NULL
        AND t.action IN ('activation', 'password_recovery')
    JOIN accounts a
    ON a.id = t.account
    AND (
      (t.action = 'activation' AND a.status = 'provisioned')
      OR (t.action = 'password_recovery' AND a.status = 'active')
    )
WHERE
    jobs.job_type = 'mailroom'
ORDER BY
    id ASC
LIMIT 10
```

**Joins & filters explained:**

- **Jobs table:** We filter for rows where `job_type` is `mailroom`.
- **Tokens table:**
  - We join tokens with jobs using the condition `tokens.id > jobs.last_seq`, which ensures we only process tokens that haven't been handled yet.
  - We further filter tokens to include only those that are not expired (`expires_at` is in the future), have not been consumed (`consumed_at` is NULL), and have an action of either `activation` or `password_recovery`.
- **Accounts table:**
  - We join accounts on `accounts.id = tokens.account`.
  - For tokens with the `activation` action, the account must be in the `provisioned` state.
  - For tokens with the `password_recovery` action, the account must be `active`.

### Dequeueing and advancing the cursor

Next, we integrate this query into a common table expression:

```sql
WITH token_data AS (
    -- Insert SELECT query here
),
updated_jobs AS (
  UPDATE
    jobs
  SET
    last_seq = (SELECT MAX(id) FROM token_data)
  WHERE
    EXISTS (SELECT 1 FROM token_data)
  RETURNING last_seq
)
SELECT
  td.action,
  td.email,
  td.login,
  td.secret,
  td.code
FROM
  token_data td
```

This accomplishes two key tasks:

1. **Retrieves tokens** generated after the current `last_seq` along with the corresponding user data.
2. **Updates the `last_seq` value** to prevent processing the same tokens again.

**Output example:**

```
-[ RECORD 1 ]--------------------------------------------------------------
action | activation
email  | usere3213152e8cdf722466a011b1eaa3c98@fake.mail
login  | user85341405cb33cbe89a5f
secret | \x144d3ba23d4e60f80d3cb5cf25783539ba267af34aecd71d7cc888643c912fb7
code   | 06435
-[ RECORD 2 ]--------------------------------------------------------------
action | activation
email  | user41e8b6830c76870594161150051f8215@fake.mail
login  | user2491d87beb8950b4abd7
secret | \x27100e07220b62e849e788e6554fede60c96e967c4aa62db7dc45150c51be23f
code   | 80252
-[ RECORD 3 ]--------------------------------------------------------------
action | activation
email  | user7bb11e235c85afe12076884d06910be4@fake.mail
login  | user91ab8536cb05c37ff46a
secret | \xa9763eec727835bd97b79018b308613268d9ea0db70493fd212771c9b7c3bcb2
code   | 31620
```

#### Indexes

To optimize the query performance, the following composite indexes are recommended:

```sql
CREATE INDEX accounts_id_status_idx ON accounts (id, status);

CREATE INDEX tokens_id_expires_consumed_action_idx ON tokens
    (id, expires_at, consumed_at, action);
```

Indexing Strategy:

- **Equality Conditions First**: Since columns used in equality conditions (`=` or `IN`) are typically the most selective, they should come first.
- **Range Conditions Next**: Columns used in range conditions (`>`, `<`, `BETWEEN`) should follow.
