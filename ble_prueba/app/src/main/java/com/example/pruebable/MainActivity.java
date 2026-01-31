package com.example.pruebable;
import android.os.Bundle;

import androidx.activity.EdgeToEdge;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;

import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import java.util.ArrayList;
import java.util.List;

/**
 * ════════════════════════════════════════════════════════════════════════
 * 📱 MainActivity - Pantalla Principal de Escaneo BLE
 * ════════════════════════════════════════════════════════════════════════
 *
 * Esta actividad permite:
 * - Escanear dispositivos BLE cercanos
 * - Filtrar dispositivos Heltec
 * - Gestionar permisos de Bluetooth y ubicación
 * - Navegar a DeviceActivity al seleccionar un dispositivo
 *
 * @author alex127845
 * @date 2025-01-21
 * @version 2.0
 */
public class MainActivity extends AppCompatActivity {

    private static final String TAG = "MainActivity";

    // ════════════════════════════════════════════════════════════════════
    // 🔧 CONSTANTES
    // ════════════════════════════════════════════════════════════════════

    private static final int REQUEST_ENABLE_BT = 1;
    private static final int REQUEST_PERMISSIONS = 2;
    private static final long SCAN_PERIOD = 10000; // 10 segundos de escaneo

    // ════════════════════════════════════════════════════════════════════
    // 🎨 COMPONENTES UI
    // ════════════════════════════════════════════════════════════════════

    private Button btnScan;
    private ProgressBar progressBar;
    private TextView tvStatus;
    private RecyclerView recyclerView;
    private DeviceAdapter deviceAdapter;

    // ════════════════════════════════════════════════════════════════════
    // 📡 BLUETOOTH
    // ════════════════════════════════════════════════════════════════════

    private BluetoothAdapter bluetoothAdapter;
    private BluetoothLeScanner bluetoothLeScanner;
    private Handler handler = new Handler();
    private boolean isScanning = false;

    // Lista de dispositivos encontrados
    private List<BluetoothDevice> deviceList = new ArrayList<>();

    // ════════════════════════════════════════════════════════════════════
    // 🚀 CICLO DE VIDA - onCreate
    // ════════════════════════════════════════════════════════════════════

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        Log.d(TAG, "📱 MainActivity iniciada");

        // Configurar ActionBar
        if (getSupportActionBar() != null) {
            getSupportActionBar().setTitle("🔍 Escanear Heltec");
        }

        // Inicializar vistas
        initViews();

        // Inicializar Bluetooth
        initBluetooth();

        // Verificar permisos
        checkPermissions();
    }

    // ════════════════════════════════════════════════════════════════════
    // 🎨 INICIALIZAR VISTAS
    // ════════════════════════════════════════════════════════════════════

    private void initViews() {
        btnScan = findViewById(R.id.btnScan);
        progressBar = findViewById(R.id.progressBar);
        tvStatus = findViewById(R.id.tvStatus);
        recyclerView = findViewById(R.id.recyclerView);

        // Configurar RecyclerView
        recyclerView.setLayoutManager(new LinearLayoutManager(this));
        deviceAdapter = new DeviceAdapter();
        recyclerView.setAdapter(deviceAdapter);

        // Configurar botón de escaneo
        btnScan.setOnClickListener(v -> {
            if (isScanning) {
                stopScan();
            } else {
                startScan();
            }
        });

        // Estado inicial
        updateUI(false, "Presiona 'Escanear' para buscar dispositivos");
    }

    // ════════════════════════════════════════════════════════════════════
    // 📡 INICIALIZAR BLUETOOTH
    // ════════════════════════════════════════════════════════════════════

    private void initBluetooth() {
        Log.d(TAG, "🔧 Inicializando Bluetooth...");

        // Obtener BluetoothManager
        BluetoothManager bluetoothManager =
                (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);

        if (bluetoothManager == null) {
            Log.e(TAG, "❌ BluetoothManager no disponible");
            showError("Bluetooth no disponible en este dispositivo");
            return;
        }

        // Obtener BluetoothAdapter
        bluetoothAdapter = bluetoothManager.getAdapter();

        if (bluetoothAdapter == null) {
            Log.e(TAG, "❌ BluetoothAdapter no disponible");
            showError("Bluetooth no soportado en este dispositivo");
            return;
        }

        // Verificar si Bluetooth está habilitado
        if (!bluetoothAdapter.isEnabled()) {
            Log.w(TAG, "⚠️ Bluetooth deshabilitado, solicitando activación");
            Intent enableBtIntent = new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE);
            if (ActivityCompat.checkSelfPermission(this,
                    Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED) {
                startActivityForResult(enableBtIntent, REQUEST_ENABLE_BT);
            }
        } else {
            Log.d(TAG, "✅ Bluetooth habilitado");
            bluetoothLeScanner = bluetoothAdapter.getBluetoothLeScanner();
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 🔐 VERIFICAR Y SOLICITAR PERMISOS
    // ════════════════════════════════════════════════════════════════════

    private void checkPermissions() {
        Log.d(TAG, "🔍 Verificando permisos...");

        List<String> permissionsNeeded = new ArrayList<>();

        // Permisos según la versión de Android
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // Android 12+ (API 31+)
            if (ContextCompat.checkSelfPermission(this,
                    Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED) {
                permissionsNeeded.add(Manifest.permission.BLUETOOTH_SCAN);
            }
            if (ContextCompat.checkSelfPermission(this,
                    Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                permissionsNeeded.add(Manifest.permission.BLUETOOTH_CONNECT);
            }
        } else {
            // Android 11 y anteriores
            if (ContextCompat.checkSelfPermission(this,
                    Manifest.permission.BLUETOOTH) != PackageManager.PERMISSION_GRANTED) {
                permissionsNeeded.add(Manifest.permission.BLUETOOTH);
            }
            if (ContextCompat.checkSelfPermission(this,
                    Manifest.permission.BLUETOOTH_ADMIN) != PackageManager.PERMISSION_GRANTED) {
                permissionsNeeded.add(Manifest.permission.BLUETOOTH_ADMIN);
            }
        }

        // Permiso de ubicación (requerido para BLE en todas las versiones)
        if (ContextCompat.checkSelfPermission(this,
                Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
            permissionsNeeded.add(Manifest.permission.ACCESS_FINE_LOCATION);
        }

        // Solicitar permisos si es necesario
        if (!permissionsNeeded.isEmpty()) {
            Log.w(TAG, "⚠️ Faltan permisos: " + permissionsNeeded.size());
            ActivityCompat.requestPermissions(
                    this,
                    permissionsNeeded.toArray(new String[0]),
                    REQUEST_PERMISSIONS
            );
        } else {
            Log.d(TAG, "✅ Todos los permisos concedidos");
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 📊 RESULTADO DE SOLICITUD DE PERMISOS
    // ════════════════════════════════════════════════════════════════════

    @Override
    public void onRequestPermissionsResult(int requestCode,
                                           @NonNull String[] permissions,
                                           @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);

        if (requestCode == REQUEST_PERMISSIONS) {
            boolean allGranted = true;

            for (int result : grantResults) {
                if (result != PackageManager.PERMISSION_GRANTED) {
                    allGranted = false;
                    break;
                }
            }

            if (allGranted) {
                Log.d(TAG, "✅ Todos los permisos concedidos");
                Toast.makeText(this, "✅ Permisos concedidos", Toast.LENGTH_SHORT).show();
            } else {
                Log.e(TAG, "❌ Permisos denegados");
                showPermissionDialog();
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 💬 DIÁLOGO DE PERMISOS
    // ════════════════════════════════════════════════════════════════════

    private void showPermissionDialog() {
        new AlertDialog.Builder(this)
                .setTitle("⚠️ Permisos Requeridos")
                .setMessage("Esta aplicación necesita permisos de Bluetooth y Ubicación " +
                        "para escanear dispositivos BLE.\n\n" +
                        "Sin estos permisos, la aplicación no puede funcionar.")
                .setPositiveButton("Intentar de nuevo", (dialog, which) -> {
                    checkPermissions();
                })
                .setNegativeButton("Salir", (dialog, which) -> {
                    finish();
                })
                .setCancelable(false)
                .show();
    }

    // ════════════════════════════════════════════════════════════════════
    // 📊 RESULTADO DE ACTIVACIÓN DE BLUETOOTH
    // ════════════════════════════════════════════════════════════════════

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == REQUEST_ENABLE_BT) {
            if (resultCode == RESULT_OK) {
                Log.d(TAG, "✅ Bluetooth habilitado por el usuario");
                Toast.makeText(this, "✅ Bluetooth habilitado", Toast.LENGTH_SHORT).show();
                bluetoothLeScanner = bluetoothAdapter.getBluetoothLeScanner();
            } else {
                Log.e(TAG, "❌ Usuario rechazó habilitar Bluetooth");
                showError("Bluetooth es necesario para usar esta aplicación");
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 🔍 INICIAR ESCANEO BLE
    // ════════════════════════════════════════════════════════════════════

    private void startScan() {
        Log.d(TAG, "🔍 Iniciando escaneo BLE...");

        // Verificar permisos
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ActivityCompat.checkSelfPermission(this,
                    Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED) {
                Log.e(TAG, "❌ Sin permiso BLUETOOTH_SCAN");
                checkPermissions();
                return;
            }
        }

        // Verificar Bluetooth habilitado
        if (bluetoothAdapter == null || !bluetoothAdapter.isEnabled()) {
            Log.e(TAG, "❌ Bluetooth no disponible o deshabilitado");
            showError("Habilita Bluetooth para escanear");
            return;
        }

        // Limpiar lista anterior
        deviceList.clear();
        deviceAdapter.notifyDataSetChanged();

        // Actualizar UI
        isScanning = true;
        updateUI(true, "Escaneando dispositivos BLE...");

        // Iniciar escaneo
        if (bluetoothLeScanner == null) {
            bluetoothLeScanner = bluetoothAdapter.getBluetoothLeScanner();
        }

        bluetoothLeScanner.startScan(scanCallback);

        // Detener escaneo automáticamente después de SCAN_PERIOD
        handler.postDelayed(() -> {
            if (isScanning) {
                stopScan();
            }
        }, SCAN_PERIOD);

        Log.d(TAG, "✅ Escaneo iniciado (duración: " + SCAN_PERIOD + "ms)");
    }

    // ════════════════════════════════════════════════════════════════════
    // ⏹️ DETENER ESCANEO BLE
    // ════════════════════════════════════════════════════════════════════

    private void stopScan() {
        if (!isScanning) return;

        Log.d(TAG, "⏹️ Deteniendo escaneo...");

        isScanning = false;

        if (bluetoothLeScanner != null) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                if (ActivityCompat.checkSelfPermission(this,
                        Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED) {
                    bluetoothLeScanner.stopScan(scanCallback);
                }
            } else {
                bluetoothLeScanner.stopScan(scanCallback);
            }
        }

        // Actualizar UI
        String status = deviceList.isEmpty()
                ? "No se encontraron dispositivos"
                : "Se encontraron " + deviceList.size() + " dispositivo(s)";

        updateUI(false, status);

        Log.d(TAG, "✅ Escaneo detenido - " + status);
    }

    // ════════════════════════════════════════════════════════════════════
    // 📡 CALLBACK DE ESCANEO BLE
    // ════════════════════════════════════════════════════════════════════

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            BluetoothDevice device = result.getDevice();

            if (device == null) return;

            // Verificar permisos para obtener nombre
            String deviceName = null;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                if (ActivityCompat.checkSelfPermission(MainActivity.this,
                        Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED) {
                    deviceName = device.getName();
                }
            } else {
                deviceName = device.getName();
            }

            // Filtrar solo dispositivos Heltec
            if (deviceName != null && deviceName.contains("Heltec")) {
                // Evitar duplicados
                if (!deviceList.contains(device)) {
                    Log.d(TAG, "✨ Dispositivo encontrado: " + deviceName +
                            " (" + device.getAddress() + ")");

                    deviceList.add(device);
                    deviceAdapter.notifyDataSetChanged();

                    // Actualizar contador en UI
                    updateUI(true, "Encontrados: " + deviceList.size() + " dispositivo(s)");
                }
            }
        }

        @Override
        public void onScanFailed(int errorCode) {
            Log.e(TAG, "❌ Error en escaneo BLE: " + errorCode);
            isScanning = false;
            updateUI(false, "Error en escaneo (código: " + errorCode + ")");
            showError("Error al escanear dispositivos BLE");
        }
    };

    // ════════════════════════════════════════════════════════════════════
    // 🎨 ACTUALIZAR UI
    // ════════════════════════════════════════════════════════════════════

    private void updateUI(boolean scanning, String status) {
        runOnUiThread(() -> {
            // Actualizar botón
            if (scanning) {
                btnScan.setText("⏹️ Detener Escaneo");
                btnScan.setEnabled(true);
                progressBar.setVisibility(View.VISIBLE);
            } else {
                btnScan.setText("🔍 Escanear Heltec");
                btnScan.setEnabled(true);
                progressBar.setVisibility(View.GONE);
            }

            // Actualizar estado
            tvStatus.setText(status);
        });
    }

    // ════════════════════════════════════════════════════════════════════
    // ❌ MOSTRAR ERROR
    // ════════════════════════════════════════════════════════════════════

    private void showError(String message) {
        runOnUiThread(() -> {
            Toast.makeText(this, "❌ " + message, Toast.LENGTH_LONG).show();
        });
    }

    // ════════════════════════════════════════════════════════════════════
    // 🔄 CICLO DE VIDA - onDestroy
    // ════════════════════════════════════════════════════════════════════

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.d(TAG, "💥 MainActivity destruida");

        // Detener escaneo si está activo
        if (isScanning) {
            stopScan();
        }

        // Limpiar handler
        handler.removeCallbacksAndMessages(null);
    }

    // ════════════════════════════════════════════════════════════════════
    // 📋 ADAPTADOR DE LISTA DE DISPOSITIVOS
    // ════════════════════════════════════════════════════════════════════

    /**
     * Adaptador para mostrar la lista de dispositivos BLE encontrados
     */
    private class DeviceAdapter extends RecyclerView.Adapter<DeviceAdapter.ViewHolder> {

        @NonNull
        @Override
        public ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            View view = LayoutInflater.from(parent.getContext())
                    .inflate(R.layout.item_device, parent, false);
            return new ViewHolder(view);
        }

        @Override
        public void onBindViewHolder(@NonNull ViewHolder holder, int position) {
            BluetoothDevice device = deviceList.get(position);

            // Obtener dirección (siempre disponible)
            final String deviceAddress = device.getAddress();

            // Obtener nombre del dispositivo
            String tempDeviceName = "Dispositivo BLE";

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                if (ActivityCompat.checkSelfPermission(MainActivity.this,
                        Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED) {
                    tempDeviceName = device.getName();
                }
            } else {
                tempDeviceName = device.getName();
            }

            if (tempDeviceName == null) {
                tempDeviceName = "Dispositivo BLE";
            }

            // Declarar como final para usar en lambda
            final String deviceName = tempDeviceName;

            // Configurar vistas
            holder.tvDeviceName.setText(deviceName);
            holder.tvDeviceAddress.setText("📍 " + deviceAddress);

            // Click para conectar
            holder.itemView.setOnClickListener(v -> {
                Log.d(TAG, "🔌 Usuario seleccionó: " + deviceName);

                // Detener escaneo
                if (isScanning) {
                    stopScan();
                }

                // Navegar a DeviceActivity
                Intent intent = new Intent(MainActivity.this, DeviceActivity.class);
                intent.putExtra("DEVICE_ADDRESS", deviceAddress);
                intent.putExtra("DEVICE_NAME", deviceName);
                startActivity(intent);
            });
        }

        @Override
        public int getItemCount() {
            return deviceList.size();
        }

        /**
         * ViewHolder para cada item de la lista
         */
        class ViewHolder extends RecyclerView.ViewHolder {
            TextView tvDeviceName;
            TextView tvDeviceAddress;

            ViewHolder(View itemView) {
                super(itemView);
                tvDeviceName = itemView.findViewById(R.id.tvDeviceName);
                tvDeviceAddress = itemView.findViewById(R.id.tvDeviceAddress);
            }
        }
    }
}