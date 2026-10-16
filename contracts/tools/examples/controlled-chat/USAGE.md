# Controlled chat

This package provides a local SQLite chat journal. It does not log into QQ or any network account.

Discover and load operations before use. `list` lists journal conversations. `read` takes a host-authorized `chat_id`, optional adapter cursor and page limit (1–256). Empty `items` means the query completed with no new messages. Keep returned event and message identities intact.

`send` writes one actual message to the controlled journal and returns `remote_message_id`, `content`, and `occurred_at`. Only the host's reply binding can authorize sending. `receipt` inspects an existing `message_id`; an absent receipt does not prove a previous write did not occur.

After a successful send, a task may propose `wait` with kind `message` and `afterStepId` equal to the actual successful step ID. The host supplies the archived outgoing message anchor. A wait may release attention to another activity; revalidate current messages when resumed.

The optional source shares the same event IDs, sequences, cursors, and messages as `read`. Keep mutable `database` outside this package. The source starts only with a host executor binding and explicit enablement.
