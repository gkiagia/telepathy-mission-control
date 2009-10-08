# Copyright (C) 2009 Nokia Corporation
# Copyright (C) 2009 Collabora Ltd.
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
# 02110-1301 USA

import dbus
"""Regression test for dispatching an incoming Text channel.
"""

import dbus
import dbus.service

from servicetest import EventPattern, tp_name_prefix, tp_path_prefix, \
        call_async, sync_dbus
from mctest import exec_test, SimulatedConnection, SimulatedClient, \
        create_fakecm_account, enable_fakecm_account, SimulatedChannel, \
        expect_client_setup
import constants as cs

text_fixed_properties = dbus.Dictionary({
    cs.CHANNEL + '.TargetHandleType': cs.HT_CONTACT,
    cs.CHANNEL + '.ChannelType': cs.CHANNEL_TYPE_TEXT,
    }, signature='sv')

def signal_channel_expect_query(q, bus, account, conn):
    # This target is special-cased in test-plugin.c
    target = 'policy@example.com'
    channel_properties = dbus.Dictionary(text_fixed_properties,
            signature='sv')
    channel_properties[cs.CHANNEL + '.TargetID'] = target
    channel_properties[cs.CHANNEL + '.TargetHandle'] = \
            conn.ensure_handle(cs.HT_CONTACT, target)
    channel_properties[cs.CHANNEL + '.InitiatorID'] = target
    channel_properties[cs.CHANNEL + '.InitiatorHandle'] = \
            conn.ensure_handle(cs.HT_CONTACT, target)
    channel_properties[cs.CHANNEL + '.Requested'] = False
    channel_properties[cs.CHANNEL + '.Interfaces'] = \
            dbus.Array([cs.CHANNEL_IFACE_DESTROYABLE, cs.CHANNEL_IFACE_GROUP,
                ],signature='s')

    chan = SimulatedChannel(conn, channel_properties, group=True)
    chan.announce()

    e = q.expect('dbus-signal',
            path=cs.CD_PATH,
            interface=cs.CD_IFACE_OP_LIST,
            signal='NewDispatchOperation')

    cdo_path = e.args[0]
    cdo_properties = e.args[1]

    assert cdo_properties[cs.CDO + '.Account'] == account.object_path
    assert cdo_properties[cs.CDO + '.Connection'] == conn.object_path
    assert cs.CDO + '.Interfaces' in cdo_properties

    handlers = cdo_properties[cs.CDO + '.PossibleHandlers'][:]
    handlers.sort()
    assert handlers == [cs.tp_name_prefix + '.Client.Empathy',
            cs.tp_name_prefix + '.Client.Kopete'], handlers

    # What does the policy service think?
    e = q.expect('dbus-method-call', path='/com/example/Policy',
            interface='com.example.Policy', method='RequestPermission')

    # Think about it for a bit
    sync_dbus(bus, q, account)

    # Let the test code decide how to reply
    return e, chan, cdo_path

def test(q, bus, mc):
    params = dbus.Dictionary({"account": "someguy@example.com",
        "password": "secrecy"}, signature='sv')
    cm_name_ref, account = create_fakecm_account(q, bus, mc, params)
    conn = enable_fakecm_account(q, bus, mc, account, params)

    policy_bus_name_ref = dbus.service.BusName('com.example.Policy', bus)

    # Throughout this entire test, we should never be asked to handle a
    # channel.
    forbidden = [
            EventPattern('dbus-method-call', method='HandleChannels'),
            ]
    q.forbid_events(forbidden)

    # Two clients want to observe, approve and handle channels
    empathy = SimulatedClient(q, bus, 'Empathy',
            observe=[text_fixed_properties], approve=[text_fixed_properties],
            handle=[text_fixed_properties], bypass_approval=False)
    kopete = SimulatedClient(q, bus, 'Kopete',
            observe=[text_fixed_properties], approve=[text_fixed_properties],
            handle=[text_fixed_properties], bypass_approval=False)

    # wait for MC to download the properties
    expect_client_setup(q, [empathy, kopete])

    # subscribe to the OperationList interface (MC assumes that until this
    # property has been retrieved once, nobody cares)

    cd = bus.get_object(cs.CD, cs.CD_PATH)
    cd_props = dbus.Interface(cd, cs.PROPERTIES_IFACE)
    assert cd_props.Get(cs.CD_IFACE_OP_LIST, 'DispatchOperations') == []

    e, chan, cdo_path = signal_channel_expect_query(q, bus, account, conn)

    # No.
    q.dbus_raise(e.message, 'com.example.Errors.No', 'Denied!')

    # The plugin responds
    _, _, e = q.expect_many(
            EventPattern('dbus-signal', path=cdo_path,
                interface=cs.CDO, signal='Finished'),
            EventPattern('dbus-signal', path=cs.CD_PATH,
                interface=cs.CD_IFACE_OP_LIST,
                signal='DispatchOperationFinished',
                args=[cdo_path]),
            EventPattern('dbus-method-call',
                path=chan.object_path,
                interface=cs.CHANNEL_IFACE_GROUP,
                # this error message is from the plugin
                method='RemoveMembersWithReason', args=[[conn.self_handle],
                    "Computer says no", cs.GROUP_REASON_PERMISSION_DENIED],
                handled=False),
            )
    q.dbus_return(e.message, signature='')
    chan.close()

    # Try again
    e, chan, cdo_path = signal_channel_expect_query(q, bus, account, conn)

    # Yes.
    q.dbus_return(e.message, signature='')

    e, k = q.expect_many(
            EventPattern('dbus-method-call',
                path=empathy.object_path,
                interface=cs.OBSERVER, method='ObserveChannels',
                handled=False),
            EventPattern('dbus-method-call',
                path=kopete.object_path,
                interface=cs.OBSERVER, method='ObserveChannels',
                handled=False),
            )
    q.dbus_return(k.message, signature='')
    q.dbus_return(e.message, signature='')

    e, k = q.expect_many(
            EventPattern('dbus-method-call',
                path=empathy.object_path,
                interface=cs.APPROVER, method='AddDispatchOperation',
                handled=False),
            EventPattern('dbus-method-call',
                path=kopete.object_path,
                interface=cs.APPROVER, method='AddDispatchOperation',
                handled=False),
            )
    q.dbus_return(e.message, signature='')
    q.dbus_return(k.message, signature='')

    kopete_cdo = bus.get_object(cs.CD, cdo_path)
    kopete_cdo_iface = dbus.Interface(kopete_cdo, cs.CDO)
    call_async(q, kopete_cdo_iface, 'Claim')

    q.expect_many(
            EventPattern('dbus-signal', path=cdo_path, signal='Finished'),
            EventPattern('dbus-signal', path=cs.CD_PATH,
                signal='DispatchOperationFinished', args=[cdo_path]),
            EventPattern('dbus-return', method='Claim'),
            )

if __name__ == '__main__':
    exec_test(test, {})