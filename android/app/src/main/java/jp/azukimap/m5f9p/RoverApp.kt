package jp.azukimap.m5f9p

import android.app.Application

class RoverApp : Application() {
    override fun onCreate() {
        super.onCreate()
        Rover.init(this)
    }
}
