package com.example.sounddetector

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.pm.PackageManager
import android.graphics.Typeface
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.util.ArrayDeque
import java.util.UUID
import kotlin.concurrent.thread
import kotlin.math.*


class MainActivity : Activity() {

    // Core detection settings.
    private val SAMPLE_RATE = 48000
    private val MAX_LAG = 21
    private val CALIBRATION_MS = 2000L
    private val ONSET_MARGIN_DB = 6.0
    private val MIN_TRIGGER_DB = -55.0
    private val EVENT_CHUNKS = 5
    private val COOLDOWN_MS = 350L
    private val CENTER_THRESHOLD = 0.16
    private val DISPLAY_CONTRAST = 0.90
    private val DISPLAY_GAMMA = 0.55

    // BLE service and three output values.
    private val SERVICE_UUID =
        UUID.fromString("12345678-1234-1234-1234-123456789000")

    private val LEFT_INTENSITY_UUID =
        UUID.fromString("12345678-1234-1234-1234-123456789001")

    private val RIGHT_INTENSITY_UUID =
        UUID.fromString("12345678-1234-1234-1234-123456789002")

    private val DIRECTION_UUID =
        UUID.fromString("12345678-1234-1234-1234-123456789003")

    private var bluetoothGatt: BluetoothGatt? = null
    private var leftCharacteristic: BluetoothGattCharacteristic? = null
    private var rightCharacteristic: BluetoothGattCharacteristic? = null
    private var directionCharacteristic: BluetoothGattCharacteristic? = null

    // BLE queue ensures LEFT, RIGHT and DIRECTION are written in order.
    private data class BleWrite(
        val characteristic: BluetoothGattCharacteristic,
        val value: Int
    )

    private val bleQueue = ArrayDeque<BleWrite>()
    private val bleLock = Any()
    private var bleWriteBusy = false

    // Compact UI.
    private lateinit var bleText: TextView
    private lateinit var liveText: TextView
    private lateinit var calibrationText: TextView
    private lateinit var channelsText: TextView
    private lateinit var timingText: TextView
    private lateinit var directionText: TextView
    private lateinit var resultText: TextView
    private lateinit var connectButton: Button
    private lateinit var detectorButton: Button

    @Volatile
    private var recording = false

    private var noiseFloorDb = -55.0


    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        createUi()
        requestNeededPermissions()
    }


    // Builds the compact diagnostic screen.
    private fun createUi() {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(40, 45, 40, 50)
        }

        fun text(value: String, size: Float = 18f) =
            TextView(this).apply {
                this.text = value
                textSize = size
                setPadding(0, 6, 0, 6)
            }

        val title = text("Sound Direction Detector", 29f).apply {
            setTypeface(null, Typeface.BOLD)
        }

        bleText = text("Arduino: Not connected")
        liveText = text("Live: -- dBFS", 21f)
        calibrationText = text("Noise: -- | Trigger: --")
        channelsText = text("LEFT: -- | RIGHT: --")
        timingText = text("TDOA: -- | Confidence: --")

        directionText = text("Direction: --", 30f).apply {
            setTypeface(null, Typeface.BOLD)
            setPadding(0, 22, 0, 10)
        }

        resultText = text(
            "LEFT intensity: --\n" +
                    "RIGHT intensity: --\n" +
                    "Score: --\n" +
                    "Output: L=--  R=--  D=--",
            20f
        )

        connectButton = Button(this).apply {
            text = "CONNECT ARDUINO"

            setOnClickListener {
                if (hasBluetoothPermissions()) {
                    scanForArduino()
                } else {
                    requestNeededPermissions()
                }
            }
        }

        detectorButton = Button(this).apply {
            text = "START MICROPHONE"

            setOnClickListener {
                if (!hasMicrophonePermission()) {
                    requestNeededPermissions()
                    return@setOnClickListener
                }

                if (!recording) {
                    recording = true
                    text = "STOP MICROPHONE"
                    startAudio()
                } else {
                    recording = false
                    text = "START MICROPHONE"

                    // Both intensities off, direction neutral.
                    sendBleValues(
                        leftIntensity = 0,
                        rightIntensity = 0,
                        direction = 128
                    )
                }
            }
        }

        root.addView(title)
        root.addView(bleText)
        root.addView(connectButton)
        root.addView(liveText)
        root.addView(calibrationText)
        root.addView(channelsText)
        root.addView(timingText)
        root.addView(directionText)
        root.addView(resultText)
        root.addView(detectorButton)

        setContentView(
            ScrollView(this).apply {
                addView(root)
            }
        )
    }


    // Requests microphone and Bluetooth permissions.
    private fun requestNeededPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            requestPermissions(
                arrayOf(
                    Manifest.permission.RECORD_AUDIO,
                    Manifest.permission.BLUETOOTH_SCAN,
                    Manifest.permission.BLUETOOTH_CONNECT
                ),
                100
            )
        } else {
            requestPermissions(
                arrayOf(
                    Manifest.permission.RECORD_AUDIO,
                    Manifest.permission.ACCESS_FINE_LOCATION
                ),
                100
            )
        }
    }


    private fun hasMicrophonePermission(): Boolean =
        checkSelfPermission(Manifest.permission.RECORD_AUDIO) ==
                PackageManager.PERMISSION_GRANTED


    private fun hasBluetoothPermissions(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return true

        return checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) ==
                PackageManager.PERMISSION_GRANTED &&
                checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) ==
                PackageManager.PERMISSION_GRANTED
    }


    // Finds SoundArduinoTest and connects to it.
    @SuppressLint("MissingPermission")
    private fun scanForArduino() {
        if (!hasBluetoothPermissions()) {
            requestNeededPermissions()
            return
        }

        val manager =
            getSystemService(BLUETOOTH_SERVICE) as BluetoothManager

        val adapter = manager.adapter ?: run {
            bleText.text = "Arduino: Bluetooth unavailable"
            return
        }

        if (!adapter.isEnabled) {
            bleText.text = "Arduino: Turn Bluetooth on"
            return
        }

        val scanner = adapter.bluetoothLeScanner ?: run {
            bleText.text = "Arduino: BLE scanner unavailable"
            return
        }

        bleText.text = "Arduino: Searching..."

        val callback = object : ScanCallback() {

            override fun onScanResult(
                callbackType: Int,
                result: ScanResult
            ) {
                val device = result.device

                val name =
                    result.scanRecord?.deviceName ?: try {
                        device.name
                    } catch (_: Exception) {
                        null
                    }

                if (name == "SoundArduinoTest") {
                    scanner.stopScan(this)

                    runOnUiThread {
                        bleText.text = "Arduino: Connecting..."
                    }

                    bluetoothGatt = device.connectGatt(
                        this@MainActivity,
                        false,
                        gattCallback
                    )
                }
            }

            override fun onScanFailed(errorCode: Int) {
                runOnUiThread {
                    bleText.text = "Arduino: Scan failed ($errorCode)"
                }
            }
        }

        scanner.startScan(callback)
    }


    // Handles connection, characteristic discovery and queued BLE writes.
    private val gattCallback = object : BluetoothGattCallback() {

        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(
            gatt: BluetoothGatt,
            status: Int,
            newState: Int
        ) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                runOnUiThread {
                    bleText.text = "Arduino: Connected..."
                }

                gatt.discoverServices()

            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                leftCharacteristic = null
                rightCharacteristic = null
                directionCharacteristic = null

                synchronized(bleLock) {
                    bleQueue.clear()
                    bleWriteBusy = false
                }

                runOnUiThread {
                    bleText.text = "Arduino: Disconnected"
                    connectButton.text = "CONNECT ARDUINO"
                    connectButton.isEnabled = true
                }
            }
        }


        override fun onServicesDiscovered(
            gatt: BluetoothGatt,
            status: Int
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                runOnUiThread {
                    bleText.text = "Arduino: Service discovery failed"
                }
                return
            }

            val service = gatt.getService(SERVICE_UUID)

            leftCharacteristic =
                service?.getCharacteristic(LEFT_INTENSITY_UUID)

            rightCharacteristic =
                service?.getCharacteristic(RIGHT_INTENSITY_UUID)

            directionCharacteristic =
                service?.getCharacteristic(DIRECTION_UUID)

            runOnUiThread {
                if (
                    leftCharacteristic != null &&
                    rightCharacteristic != null &&
                    directionCharacteristic != null
                ) {
                    bleText.text = "Arduino: READY ✓"
                    connectButton.text = "CONNECTED"
                    connectButton.isEnabled = false
                } else {
                    bleText.text = "Arduino: BLE characteristics missing"
                }
            }
        }


        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            writeNextBleValue()
        }
    }


    // Queues LEFT, RIGHT and DIRECTION so all three belong to the same event.
    private fun sendBleValues(
        leftIntensity: Int,
        rightIntensity: Int,
        direction: Int
    ) {
        val left = leftCharacteristic ?: return
        val right = rightCharacteristic ?: return
        val dir = directionCharacteristic ?: return

        var startQueue = false

        synchronized(bleLock) {
            bleQueue.add(
                BleWrite(
                    left,
                    leftIntensity.coerceIn(0, 255)
                )
            )

            bleQueue.add(
                BleWrite(
                    right,
                    rightIntensity.coerceIn(0, 255)
                )
            )

            bleQueue.add(
                BleWrite(
                    dir,
                    direction.coerceIn(0, 255)
                )
            )

            if (!bleWriteBusy) {
                bleWriteBusy = true
                startQueue = true
            }
        }

        if (startQueue) {
            writeNextBleValue()
        }
    }


    // Writes one queued BLE value and waits for its callback before continuing.
    @SuppressLint("MissingPermission")
    private fun writeNextBleValue() {
        val item: BleWrite

        synchronized(bleLock) {
            if (bleQueue.isEmpty()) {
                bleWriteBusy = false
                return
            }

            item = bleQueue.removeFirst()
        }

        val gatt = bluetoothGatt ?: run {
            synchronized(bleLock) {
                bleQueue.clear()
                bleWriteBusy = false
            }
            return
        }

        val data =
            byteArrayOf(item.value.toByte())

        val started =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                gatt.writeCharacteristic(
                    item.characteristic,
                    data,
                    BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                ) == BluetoothStatusCodes.SUCCESS
            } else {
                item.characteristic.writeType =
                    BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT

                item.characteristic.value = data

                gatt.writeCharacteristic(
                    item.characteristic
                )
            }

        if (!started) {
            writeNextBleValue()
        }
    }


    // Records stereo audio, calibrates noise and captures sound onsets.
    @SuppressLint("MissingPermission")
    private fun startAudio() {
        thread {
            val minBuffer = AudioRecord.getMinBufferSize(
                SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_STEREO,
                AudioFormat.ENCODING_PCM_16BIT
            )

            if (minBuffer <= 0) {
                stopWithError("Stereo recording unavailable")
                return@thread
            }

            val format = AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(SAMPLE_RATE)
                .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                .build()

            val recorder = try {
                AudioRecord.Builder()
                    .setAudioSource(MediaRecorder.AudioSource.MIC)
                    .setAudioFormat(format)
                    .setBufferSizeInBytes(
                        maxOf(minBuffer * 2, 16384)
                    )
                    .build()
            } catch (_: Exception) {
                null
            }

            if (
                recorder == null ||
                recorder.state != AudioRecord.STATE_INITIALIZED
            ) {
                recorder?.release()
                stopWithError("Could not open Fold 5 microphones")
                return@thread
            }

            recorder.startRecording()

            val buffer = ShortArray(1024)

            // Measures the frozen room noise floor for two seconds.
            val calibration = ArrayList<Double>()

            val calibrationEnd =
                System.currentTimeMillis() +
                        CALIBRATION_MS

            runOnUiThread {
                directionText.text = "CALIBRATING..."
            }

            while (
                recording &&
                System.currentTimeMillis() < calibrationEnd
            ) {
                val count =
                    recorder.read(
                        buffer,
                        0,
                        buffer.size
                    )

                if (count <= 0) continue

                val db =
                    getOverallDb(
                        buffer,
                        count
                    )

                if (db.isFinite()) {
                    calibration.add(db)
                }

                runOnUiThread {
                    liveText.text =
                        "Live: %.1f dBFS".format(db)
                }
            }

            noiseFloorDb =
                if (calibration.isNotEmpty()) {
                    calibration.sort()

                    calibration[
                        calibration.size / 2
                    ].coerceIn(
                        -70.0,
                        -25.0
                    )
                } else {
                    -55.0
                }

            val triggerDb =
                max(
                    MIN_TRIGGER_DB,
                    noiseFloorDb +
                            ONSET_MARGIN_DB
                )

            runOnUiThread {
                calibrationText.text =
                    "Noise: %.1f | Trigger: %.1f dBFS"
                        .format(
                            noiseFloorDb,
                            triggerDb
                        )

                directionText.text =
                    "Direction: Waiting..."
            }

            var armed = true
            var belowCounter = 0
            var eventActive = false
            var eventChunks = 0
            var cooldownUntil = 0L

            val eventRight =
                ArrayList<Double>()

            val eventLeft =
                ArrayList<Double>()

            var previousRight =
                DoubleArray(0)

            var previousLeft =
                DoubleArray(0)

            // Monitors continuously and saves only the beginning of each event.
            while (recording) {
                val count =
                    recorder.read(
                        buffer,
                        0,
                        buffer.size
                    )

                if (count <= 0) continue

                val frames =
                    count / 2

                if (frames <= 0) continue

                val right =
                    DoubleArray(frames)

                val left =
                    DoubleArray(frames)

                for (i in 0 until frames) {

                    // Channel A = RIGHT.
                    right[i] =
                        buffer[
                            i * 2
                        ].toDouble()

                    // Channel B = LEFT.
                    left[i] =
                        buffer[
                            i * 2 + 1
                        ].toDouble()
                }

                removeMean(right)
                removeMean(left)

                val rmsRight =
                    calculateRms(right)

                val rmsLeft =
                    calculateRms(left)

                val liveDb =
                    rmsToDb(
                        (
                                rmsRight +
                                        rmsLeft
                                ) / 2.0
                    )

                runOnUiThread {
                    liveText.text =
                        "Live: %.1f dBFS"
                            .format(liveDb)
                }

                if (
                    liveDb <
                    triggerDb - 3.0
                ) {
                    belowCounter++

                    if (belowCounter >= 4) {
                        armed = true
                    }

                } else {
                    belowCounter = 0
                }

                if (
                    !eventActive &&
                    armed &&
                    liveDb >= triggerDb &&
                    System.currentTimeMillis() >=
                    cooldownUntil
                ) {
                    eventActive = true
                    armed = false
                    eventChunks = 0

                    eventRight.clear()
                    eventLeft.clear()

                    for (value in previousRight) {
                        eventRight.add(value)
                    }

                    for (value in previousLeft) {
                        eventLeft.add(value)
                    }
                }

                if (eventActive) {
                    for (value in right) {
                        eventRight.add(value)
                    }

                    for (value in left) {
                        eventLeft.add(value)
                    }

                    eventChunks++

                    if (
                        eventChunks >=
                        EVENT_CHUNKS
                    ) {
                        eventActive = false

                        cooldownUntil =
                            System.currentTimeMillis() +
                                    COOLDOWN_MS

                        analyzeEvent(
                            eventRight.toDoubleArray(),
                            eventLeft.toDoubleArray(),
                            triggerDb
                        )
                    }
                }

                previousRight =
                    right.copyOf()

                previousLeft =
                    left.copyOf()
            }

            try {
                recorder.stop()
            } catch (_: Exception) {
            }

            recorder.release()

            runOnUiThread {
                detectorButton.text =
                    "START MICROPHONE"
            }
        }
    }


    // Calculates LEFT intensity, RIGHT intensity and the unchanged source direction.
    private fun analyzeEvent(
        rightInput: DoubleArray,
        leftInput: DoubleArray,
        triggerDb: Double
    ) {
        if (
            rightInput.isEmpty() ||
            leftInput.isEmpty()
        ) return

        val right =
            rightInput.copyOf()

        val left =
            leftInput.copyOf()

        removeMean(right)
        removeMean(left)

        val rmsRight =
            calculateRms(right)

        val rmsLeft =
            calculateRms(left)

        val dbRight =
            rmsToDb(rmsRight)

        val dbLeft =
            rmsToDb(rmsLeft)

        // Positive means RIGHT is louder and negative means LEFT is louder.
        val amplitudeDifference =
            dbRight - dbLeft

        val amplitudeScore =
            (
                    amplitudeDifference /
                            10.0
                    ).coerceIn(
                    -1.0,
                    1.0
                )

        val gccRight =
            right.copyOf()

        val gccLeft =
            left.copyOf()

        applyHannWindow(gccRight)
        applyHannWindow(gccLeft)

        val (lag, confidence) =
            gccPhatDelay(
                gccRight,
                gccLeft,
                MAX_LAG
            )

        // Positive means RIGHT first and negative means LEFT first.
        var timingScore =
            lag.toDouble() /
                    MAX_LAG.toDouble()

        if (confidence < 1.10) {
            timingScore = 0.0
        }

        val signalsAgree =
            timingScore == 0.0 ||
                    amplitudeScore == 0.0 ||
                    sign(timingScore) ==
                    sign(amplitudeScore)

        // Original confidence-based timing/amplitude fusion remains unchanged.
        var score =
            when {
                confidence >= 1.40 &&
                        signalsAgree ->

                    0.80 * timingScore +
                            0.20 * amplitudeScore

                confidence >= 1.15 &&
                        signalsAgree ->

                    0.60 * timingScore +
                            0.40 * amplitudeScore

                else ->

                    0.20 * timingScore +
                            0.80 * amplitudeScore
            }

        score =
            score.coerceIn(
                -1.0,
                1.0
            )

        if (
            abs(amplitudeDifference) < 1.0 &&
            abs(lag) <= 3
        ) {
            score = 0.0
        }

        val direction =
            when {
                score < -CENTER_THRESHOLD ->
                    "← LEFT"

                score > CENTER_THRESHOLD ->
                    "RIGHT →"

                else ->
                    "CENTER"
            }

        // Converts each microphone level independently to 0–255.
        val rawLeftIntensity =
            intensityFromDb(
                dbLeft,
                triggerDb
            )

        val rawRightIntensity =
            intensityFromDb(
                dbRight,
                triggerDb
            )

        val (leftIntensity, rightIntensity) =
            displayIntensities(
                rawLeftIntensity,
                rawRightIntensity,
                score
            )

        // Converts the unchanged direction score from -1..+1 into 0..255.
        val directionByte =
            (
                    (
                            score + 1.0
                            ) /
                            2.0 *
                            255.0
                    )
                .roundToInt()
                .coerceIn(
                    0,
                    255
                )

        sendBleValues(
            leftIntensity,
            rightIntensity,
            directionByte
        )

        val leftPercent =
            (
                    leftIntensity /
                            255.0 *
                            100.0
                    ).roundToInt()

        val rightPercent =
            (
                    rightIntensity /
                            255.0 *
                            100.0
                    ).roundToInt()

        val timingSide =
            when {
                lag > 1 ->
                    "RIGHT $lag samples"

                lag < -1 ->
                    "LEFT ${abs(lag)} samples"

                else ->
                    "Simultaneous"
            }

        runOnUiThread {
            channelsText.text =
                "LEFT: %.1f dBFS | RIGHT: %.1f dBFS"
                    .format(
                        dbLeft,
                        dbRight
                    )

            timingText.text =
                "TDOA: $timingSide | Confidence: %.2f"
                    .format(confidence)

            directionText.text =
                "Direction: $direction"

            resultText.text =
                "LEFT intensity: $leftPercent%  ($leftIntensity)\n" +
                        "RIGHT intensity: $rightPercent%  ($rightIntensity)\n" +
                        "Score: %.2f\n".format(score) +
                        "Output: L=$leftIntensity  R=$rightIntensity  D=$directionByte"
        }
    }


    // Enhances left/right separation on the LED display.
    private fun displayIntensities(
        left: Int,
        right: Int,
        score: Double
    ): Pair<Int, Int> {
        val base =
            max(left, right)

        if (base == 0) {
            return 0 to 0
        }

        if (abs(score) <= CENTER_THRESHOLD) {
            return base to base
        }

        val normalized =
            (
                    (abs(score) - CENTER_THRESHOLD) /
                            (1.0 - CENTER_THRESHOLD)
                    ).coerceIn(
                    0.0,
                    1.0
                )

        val strength =
            normalized.pow(
                DISPLAY_GAMMA
            )

        val weakerSide =
            (
                    base *
                            (
                                    1.0 -
                                            DISPLAY_CONTRAST *
                                            strength
                                    )
                    )
                .roundToInt()
                .coerceIn(
                    0,
                    255
                )

        return if (score < 0.0) {
            base to weakerSide
        } else {
            weakerSide to base
        }
    }


    // Maps one microphone's dBFS level to its own 0–255 intensity.
    private fun intensityFromDb(
        db: Double,
        triggerDb: Double
    ): Int {
        return (
                (
                        db -
                                triggerDb +
                                3.0
                        ) /
                        25.0 *
                        255.0
                )
            .roundToInt()
            .coerceIn(
                0,
                255
            )
    }


    // Calculates the current stereo RMS level for event triggering.
    private fun getOverallDb(
        data: ShortArray,
        count: Int
    ): Double {
        val frames =
            count / 2

        if (frames <= 0) {
            return -100.0
        }

        var rightPower = 0.0
        var leftPower = 0.0

        for (i in 0 until frames) {
            val right =
                data[
                    i * 2
                ].toDouble()

            val left =
                data[
                    i * 2 + 1
                ].toDouble()

            rightPower +=
                right * right

            leftPower +=
                left * left
        }

        val rmsRight =
            sqrt(
                rightPower /
                        frames
            )

        val rmsLeft =
            sqrt(
                leftPower /
                        frames
            )

        return rmsToDb(
            (
                    rmsRight +
                            rmsLeft
                    ) / 2.0
        )
    }


    // GCC-PHAT finds the strongest valid LEFT/RIGHT arrival-time difference.
    private fun gccPhatDelay(
        right: DoubleArray,
        left: DoubleArray,
        maxLag: Int
    ): Pair<Int, Double> {
        var fftSize = 1

        while (
            fftSize <
            right.size * 2
        ) {
            fftSize *= 2
        }

        val realR =
            DoubleArray(fftSize)

        val imagR =
            DoubleArray(fftSize)

        val realL =
            DoubleArray(fftSize)

        val imagL =
            DoubleArray(fftSize)

        for (i in right.indices) {
            realR[i] = right[i]
            realL[i] = left[i]
        }

        fft(
            realR,
            imagR,
            false
        )

        fft(
            realL,
            imagL,
            false
        )

        val crossReal =
            DoubleArray(fftSize)

        val crossImag =
            DoubleArray(fftSize)

        // Uses conj(RIGHT) × LEFT exactly as before.
        for (i in 0 until fftSize) {
            val real =
                realR[i] *
                        realL[i] +
                        imagR[i] *
                        imagL[i]

            val imag =
                realR[i] *
                        imagL[i] -
                        imagR[i] *
                        realL[i]

            val magnitude =
                sqrt(
                    real * real +
                            imag * imag
                )

            if (magnitude > 1e-12) {
                crossReal[i] =
                    real / magnitude

                crossImag[i] =
                    imag / magnitude
            }
        }

        fft(
            crossReal,
            crossImag,
            true
        )

        var bestLag = 0
        var bestPeak = -1.0

        val peaks =
            ArrayList<Pair<Int, Double>>()

        for (
        lag in -maxLag..maxLag
        ) {
            val index =
                if (lag >= 0) {
                    lag
                } else {
                    fftSize + lag
                }

            val value =
                abs(
                    crossReal[index]
                )

            peaks.add(
                lag to value
            )

            if (value > bestPeak) {
                bestPeak = value
                bestLag = lag
            }
        }

        var secondPeak = 0.0

        for (
        (lag, value) in peaks
        ) {
            if (
                abs(
                    lag - bestLag
                ) <= 2
            ) continue

            if (
                value > secondPeak
            ) {
                secondPeak = value
            }
        }

        val confidence =
            bestPeak /
                    (
                            secondPeak +
                                    1e-9
                            )

        return bestLag to confidence
    }


    // Radix-2 FFT/IFFT used by GCC-PHAT.
    private fun fft(
        real: DoubleArray,
        imag: DoubleArray,
        inverse: Boolean
    ) {
        val n = real.size
        var j = 0

        for (i in 1 until n) {
            var bit =
                n shr 1

            while (
                j and bit != 0
            ) {
                j =
                    j xor bit

                bit =
                    bit shr 1
            }

            j =
                j xor bit

            if (i < j) {
                var temp =
                    real[i]

                real[i] =
                    real[j]

                real[j] =
                    temp

                temp =
                    imag[i]

                imag[i] =
                    imag[j]

                imag[j] =
                    temp
            }
        }

        var length = 2

        while (length <= n) {
            val angle =
                2.0 *
                        Math.PI /
                        length *
                        if (inverse) {
                            1.0
                        } else {
                            -1.0
                        }

            val wlReal =
                cos(angle)

            val wlImag =
                sin(angle)

            var start = 0

            while (start < n) {
                var wr = 1.0
                var wi = 0.0

                for (
                k in 0 until
                        length / 2
                ) {
                    val even =
                        start + k

                    val odd =
                        even +
                                length / 2

                    val oddReal =
                        real[odd] *
                                wr -
                                imag[odd] *
                                wi

                    val oddImag =
                        real[odd] *
                                wi +
                                imag[odd] *
                                wr

                    val evenReal =
                        real[even]

                    val evenImag =
                        imag[even]

                    real[even] =
                        evenReal +
                                oddReal

                    imag[even] =
                        evenImag +
                                oddImag

                    real[odd] =
                        evenReal -
                                oddReal

                    imag[odd] =
                        evenImag -
                                oddImag

                    val newWr =
                        wr *
                                wlReal -
                                wi *
                                wlImag

                    val newWi =
                        wr *
                                wlImag +
                                wi *
                                wlReal

                    wr = newWr
                    wi = newWi
                }

                start +=
                    length
            }

            length *= 2
        }

        if (inverse) {
            for (i in 0 until n) {
                real[i] /= n
                imag[i] /= n
            }
        }
    }


    // Applies a Hann window before the GCC-PHAT FFT.
    private fun applyHannWindow(
        signal: DoubleArray
    ) {
        if (signal.size <= 1) return

        for (i in signal.indices) {
            val window =
                0.5 *
                        (
                                1.0 -
                                        cos(
                                            2.0 *
                                                    Math.PI *
                                                    i /
                                                    (
                                                            signal.size - 1
                                                            )
                                        )
                                )

            signal[i] *= window
        }
    }


    // Removes microphone DC offset before analysis.
    private fun removeMean(
        signal: DoubleArray
    ) {
        if (signal.isEmpty()) return

        val mean =
            signal.average()

        for (i in signal.indices) {
            signal[i] -= mean
        }
    }


    // Calculates RMS signal magnitude.
    private fun calculateRms(
        signal: DoubleArray
    ): Double {
        if (signal.isEmpty()) {
            return 0.0
        }

        var sum = 0.0

        for (value in signal) {
            sum +=
                value * value
        }

        return sqrt(
            sum /
                    signal.size
        )
    }


    // Converts 16-bit PCM RMS to dBFS.
    private fun rmsToDb(
        rms: Double
    ): Double {
        if (rms <= 0.0) {
            return -100.0
        }

        return 20.0 *
                log10(
                    rms /
                            32767.0
                )
    }


    private fun stopWithError(
        message: String
    ) {
        recording = false

        runOnUiThread {
            directionText.text =
                message

            detectorButton.text =
                "START MICROPHONE"
        }
    }


    // Stops recording and Bluetooth when the Activity closes.
    @SuppressLint("MissingPermission")
    override fun onDestroy() {
        recording = false

        if (hasBluetoothPermissions()) {
            bluetoothGatt?.disconnect()
            bluetoothGatt?.close()
        }

        bluetoothGatt = null

        super.onDestroy()
    }
}
