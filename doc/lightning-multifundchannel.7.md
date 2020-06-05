lightning-multifundchannel -- Command for establishing many lightning channels
==============================================================================

SYNOPSIS
--------

**multifundchannel** *destinations* \[*feerate*\] \[*minconf*\] \[*utxos*\]

DESCRIPTION
-----------

The **multifundchannel** RPC command opens multiple payment channels
with nodes by committing a single funding transaction to the blockchain
that is shared by all channels.

If not already connected, **multifundchannel** will automatically attempt
to connect; you may provide a *@host:port* hint appended to the node ID
so that C-lightning can learn how to connect to the node;
see lightning-connect(7).

Once the transaction is confirmed, normal channel operations may begin.
Readiness is indicated by **listpeers** reporting a *state* of
CHANNELD\_NORMAL for the channel.

*destinations* is an array of objects, with the fields:

* *id* is the node ID, with an optional *@host:port* appended to it
  in a manner understood by **connect**; see lightning-connect(7).
  Each entry in the *destinations* array must have a unique node *id*.
* *amount* is the amount in satoshis taken from the internal wallet
  to fund the channel.
  The string *all* can be used to specify all available funds
  (or 16,777,215 satoshi if more is available and large channels were
  not negotiated with the peer).
  Otherwise it is in satoshi precision; it can be
   a whole number,
   a whole number ending in *sat*,
   a whole number ending in *000msat*, or
   a number with 1 to 8 decimal places ending in *btc*.
  The value cannot be less than the dust limit, currently 546 satoshi
  as of this writing, nor more than 16,777,215 satoshi
  (unless large channels were negotiated with the peer).
* *announce* is an optional flag that indicates whether to announce
  the channel with this, default `true`.
  If set to `false`, the channel is unpublished.
* *push\_msat* is the amount of millisatoshis to outright give to the
  node.
  This is a gift to the peer, and you do not get a proof-of-payment
  out of this.

There must be at least one entry in *destinations*;
it cannot be an empty array.

*feerate* is an optional feerate used for the opening transaction and as
initial feerate for commitment and HTLC transactions. It can be one of
the strings *urgent* (aim for next block), *normal* (next 4 blocks or
so) or *slow* (next 100 blocks or so) to use lightningd’s internal
estimates: *normal* is the default.

Otherwise, *feerate* is a number, with an optional suffix: *perkw* means
the number is interpreted as satoshi-per-kilosipa (weight), and *perkb*
means it is interpreted bitcoind-style as satoshi-per-kilobyte. Omitting
the suffix is equivalent to *perkb*.

*minconf* specifies the minimum number of confirmations that used
outputs should have. Default is 1.

*utxos* specifies the utxos to be used to fund the channel, as an array
of "txid:vout".

RETURN VALUE
------------

On success, the *tx* and *txid* of the signed and broadcsted funding
transaction is returned.
This command opens multiple channels with a single large transaction,
thus only one transaction is returned.

An array of *channel\_id* is returned;
each entry of the array is a string of the long channel ID of the
channel going to the corresponding *destinations* entry.

On failure, none of the channels are created.

The following error codes may occur:
* -1: Catchall nonspecific error.
- 300: The maximum allowed funding amount is exceeded.
- 301: There are not enough funds in the internal wallet (including fees) to create the transaction.
- 302: The output amount is too small, and would be considered dust.
- 303: Broadcasting of the funding transaction failed, the internal call to bitcoin-cli returned with an error.

Failure may also occur if **lightningd** and the peer cannot agree on
channel parameters (funding limits, channel reserves, fees, etc.).
See lightning-fundchannel\_start(7) and lightning-fundchannel\_complete(7).

AUTHOR
------

ZmnSCPxj <<ZmnSCPxj@protonmail.com>> is mainly responsible.

SEE ALSO
--------

lightning-connect(7), lightning-listfunds(), lightning-listpeers(7),
lightning-fundchannel(7)

RESOURCES
---------

Main web site: <https://github.com/ElementsProject/lightning>
