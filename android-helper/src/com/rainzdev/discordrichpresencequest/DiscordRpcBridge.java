package com.rainzdev.discordrichpresencequest;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

public final class DiscordRpcBridge {
    private static final String TAG = "DRPQuestBridge";
    private static final String SERVICE_DESCRIPTOR = "com.discord.socialsdk.rpc.IDiscordRpcService";
    private static final String CONNECTION_DESCRIPTOR = "com.discord.socialsdk.rpc.IDiscordRpcConnection";
    private static final String CALLBACK_DESCRIPTOR = "com.discord.socialsdk.rpc.IDiscordRpcCallback";
    private static final String SERVICE_ACTION = "com.discord.socialsdk.rpc.IDiscordRpcService";

    private final Context context;
    private volatile IBinder connection;
    private volatile String state = "DISCONNECTED";
    private volatile String lastError = "";
    private volatile String pendingFrame;
    // ServiceConnection callbacks may run on Android's main thread while JNI
    // calls arrive from a native worker, so every cross-thread field needs
    // either volatile visibility or synchronization on this bridge instance.
    private volatile boolean bound;
    private volatile long applicationId;
    private volatile String version;

    private final Binder callback = new Binder() {
        {
            attachInterface((IInterface) null, CALLBACK_DESCRIPTOR);
        }

        @Override
        public boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code == INTERFACE_TRANSACTION) {
                // One-way Binder calls are allowed to omit a reply parcel.
                if (reply != null) reply.writeString(CALLBACK_DESCRIPTOR);
                return true;
            }
            if (code == 1) {
                data.enforceInterface(CALLBACK_DESCRIPTOR);
                String frame = data.readString();
                handleIncomingFrame(frame);
                if (reply != null) reply.writeNoException();
                return true;
            }
            if (code == 2) {
                data.enforceInterface(CALLBACK_DESCRIPTOR);
                int closeCode = data.readInt();
                String message = data.readString();
                connection = null;
                state = "CLOSED";
                lastError = closeCode + ": " + (message == null ? "" : message);
                Log.w(TAG, "Discord RPC closed: " + lastError);
                if (reply != null) reply.writeNoException();
                return true;
            }
            return super.onTransact(code, data, reply, flags);
        }
    };

    private final ServiceConnection serviceConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            // A null service or a callback delivered after unbind must not be
            // passed to transact; either case is a recoverable binding failure.
            if (service == null) {
                releaseTerminalBinding("Discord connected without providing an RPC binder");
                return;
            }
            if (!bound) {
                Log.w(TAG, "Ignoring Discord connection delivered after unbind");
                return;
            }
            try {
                IBinder connected;
                Parcel data = Parcel.obtain();
                Parcel reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(SERVICE_DESCRIPTOR);
                    data.writeLong(applicationId);
                    data.writeString(version);
                    data.writeStrongBinder(callback);
                    if (!service.transact(1, data, reply, 0)) {
                        throw new RemoteException("Discord rejected the connect transaction");
                    }
                    reply.readException();
                    connected = reply.readStrongBinder();
                } finally {
                    reply.recycle();
                    data.recycle();
                }

                if (connected == null) {
                    fail("Discord refused the RPC connection");
                    return;
                }
                synchronized (DiscordRpcBridge.this) {
                    // Do not publish the binder until this synchronized block.
                    // Publishing it immediately after the handshake allowed a
                    // native send to overtake pendingFrame; this callback would
                    // then send the older pending activity last and make Discord
                    // visibly move backward to stale state.
                    if (!bound) {
                        return;
                    }
                    connection = connected;
                    state = "CONNECTED";
                    String queued = pendingFrame;
                    pendingFrame = null;
                    // Java monitors are reentrant, so keeping the pending send
                    // inside the same critical section safely calls the
                    // synchronized method while preventing a newer native frame
                    // from being delivered first.
                    if (queued != null) sendFrame(queued);
                }
            } catch (Exception error) {
                connection = null;
                fail("Connect failed: " + error);
            }
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            synchronized (DiscordRpcBridge.this) {
                connection = null;
                // Android still owns this binding and may reconnect it
                // automatically. Keeping bound=true prevents connect() from
                // creating a second binding with the same ServiceConnection.
                state = "DISCONNECTED";
            }
        }

        @Override
        public void onBindingDied(ComponentName name) {
            // A dead binding is terminal and Android requires an explicit
            // unbind before this ServiceConnection can be rebound cleanly.
            releaseTerminalBinding("Discord RPC binding died");
        }

        @Override
        public void onNullBinding(ComponentName name) {
            releaseTerminalBinding("Discord returned a null RPC binding");
        }
    };

    public DiscordRpcBridge(Context context) {
        // Some test or vendor contexts return null here. Retain the validated
        // Activity context rather than storing a null reference for bindService.
        Context applicationContext = context.getApplicationContext();
        this.context = applicationContext != null ? applicationContext : context;
    }

    public synchronized boolean connect(long applicationId, String version) {
        if (connection != null || (bound && "BINDING".equals(state))) return true;

        // A failed/dead binding can leave Android's bound flag set without a
        // usable connection. Remove it before retrying so Reconnect can work.
        if (bound) {
            bound = false;
            try {
                context.unbindService(serviceConnection);
            } catch (IllegalArgumentException error) {
                Log.w(TAG, "Stale Discord binding was already removed", error);
            }
        }
        this.applicationId = applicationId;
        this.version = version == null ? "1" : version;
        this.lastError = "";
        this.state = "BINDING";

        Intent intent = new Intent(SERVICE_ACTION);
        intent.setComponent(new ComponentName("com.discord", "com.discord.socialrpc.DiscordRpcService"));
        try {
            bound = context.bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE);
            if (!bound) fail("Discord RPC service was not found. Install Discord and apply the Beat Saber package-visibility patch.");
            return bound;
        } catch (SecurityException | IllegalArgumentException error) {
            bound = false;
            fail("Could not bind Discord RPC service: " + error);
            return false;
        }
    }

    public synchronized boolean sendFrame(String frame) {
        IBinder target = connection;
        if (target == null) {
            pendingFrame = frame;
            return bound;
        }
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(CONNECTION_DESCRIPTOR);
            data.writeString(frame);
            if (!target.transact(1, data, reply, 0)) throw new RemoteException("Discord rejected the frame");
            reply.readException();
            return true;
        } catch (RemoteException | RuntimeException error) {
            // Drop the dead connection so the next native send can reconnect
            // instead of repeatedly transacting on the same failed binder.
            connection = null;
            fail("Send failed: " + error);
            return false;
        } finally {
            reply.recycle();
            data.recycle();
        }
    }

    public synchronized void disconnect() {
        IBinder target = connection;
        // Clear bound before IPC so a late ServiceConnection callback cannot
        // repopulate connection while shutdown is already in progress.
        boolean wasBound = bound;
        bound = false;
        connection = null;
        pendingFrame = null;
        if (target != null) {
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(CONNECTION_DESCRIPTOR);
                target.transact(2, data, reply, 0);
                reply.readException();
            } catch (RemoteException | RuntimeException error) {
                Log.w(TAG, "Disconnect transaction failed", error);
            } finally {
                reply.recycle();
                data.recycle();
            }
        }
        if (wasBound) {
            try {
                context.unbindService(serviceConnection);
            } catch (IllegalArgumentException error) {
                Log.w(TAG, "Unbind failed", error);
            }
        }
        state = "DISCONNECTED";
    }

    public String getState() {
        return state;
    }

    public String getLastError() {
        return lastError;
    }

    private void handleIncomingFrame(String frame) {
        if (frame == null) {
            lastError = "Discord sent a null RPC callback frame";
            Log.w(TAG, lastError);
            return;
        }

        try {
            // Parsing the JSON is resilient to whitespace and field ordering,
            // unlike the previous raw substring check for the READY event.
            JSONObject message = new JSONObject(frame);
            String event = message.optString("evt", "");
            if ("READY".equals(event)) {
                state = "READY";
                Log.d(TAG, "Discord RPC is ready");
                return;
            }
            if ("ERROR".equals(event)) {
                JSONObject details = message.optJSONObject("data");
                String code = details == null ? "" : details.optString("code", "");
                String text = details == null ? "" : details.optString("message", "");
                lastError = "Discord ERROR" + (code.isEmpty() ? "" : " " + code) +
                    (text.isEmpty() ? ": " + frame : ": " + text);
                // Do not mark the Binder connection itself as failed: Discord can
                // reject one activity while the service remains healthy. Native
                // code polls this diagnostic on the next presence update.
                Log.w(TAG, lastError);
                return;
            }
            Log.d(TAG, "Discord frame: " + frame);
        } catch (JSONException error) {
            lastError = "Could not parse Discord callback frame: " + error + "; frame=" + frame;
            Log.w(TAG, lastError);
        }
    }

    private void releaseTerminalBinding(String message) {
        boolean shouldUnbind;
        synchronized (this) {
            connection = null;
            shouldUnbind = bound;
            // Clear this before unbindService because Android may deliver a late
            // callback; onServiceConnected will then reject that stale callback.
            bound = false;
            fail(message);
        }
        if (shouldUnbind) {
            try {
                context.unbindService(serviceConnection);
            } catch (IllegalArgumentException error) {
                // The framework may already have removed a terminal binding.
                // This is safe, but log it so bookkeeping problems are visible.
                Log.w(TAG, "Terminal Discord binding was already removed", error);
            }
        }
    }

    private void fail(String message) {
        lastError = message;
        state = "ERROR";
        Log.e(TAG, message);
    }
}
